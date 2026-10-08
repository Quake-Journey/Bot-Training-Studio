"""Measure a real optimizer workload, not a guessed model-to-VRAM mapping."""
import gc
import time

from .learning import device_for
from .models import PROFILES, create
from .sequences import INPUTS
from .temporal import OUTPUTS, precision


def calibrate(profile,backend,context=None,event=None,cancelled=None,family='sequences'):
    import torch
    if profile=='reference':raise ValueError('Choose a temporal model profile')
    context=context or PROFILES[profile].context
    if not 1<=context<=PROFILES[profile].context:raise ValueError('Context exceeds profile')
    torch.set_num_threads(4)
    device,name=device_for(backend);rows=[]
    if family=='decisions':
        from .decisions import INPUTS as inputs
        from .decision_learning import OUTPUTS as outputs
    elif family=='sequences':
        inputs,outputs=INPUTS,OUTPUTS
    elif family=='mechanisms':
        from .mechanics_sequences import INPUTS as inputs
        outputs=8
    else:raise ValueError('Unknown model family')
    model=create(profile,len(inputs),outputs).to(device)
    model.activation_checkpointing=profile in ('large','xl')
    optimizer=torch.optim.AdamW(model.parameters(),foreach=False)
    budget=torch.cuda.mem_get_info()[0]*.7 if device.type=='cuda' else None
    casts=precision(torch,device)
    sizes=(4,8,16) if device.type=='cpu' else (8,32,128,512,1024)
    try:
        for batch in sizes:
            if cancelled and cancelled():raise InterruptedError('Resource calibration cancelled')
            try:
                optimizer.zero_grad(set_to_none=True)
                if device.type=='cuda':torch.cuda.reset_peak_memory_stats();torch.cuda.synchronize()
                start=time.perf_counter()
                x=torch.randn(batch,context,len(inputs),device=device)
                with casts():loss=model(x).square().mean()
                loss.backward();optimizer.step()
                if device.type=='cuda':torch.cuda.synchronize()
                seconds=time.perf_counter()-start
                peak=torch.cuda.max_memory_allocated() if device.type=='cuda' else None
                row=dict(batch=batch,seconds=seconds,samples_per_second=batch/seconds,peak_bytes=peak,
                         within_reserve=peak is None or peak<=budget)
                rows.append(row)
                if event:event(dict(stage='calibration',profile=profile,**row))
                del x,loss
                if peak and peak>budget:break
            except torch.OutOfMemoryError:
                rows.append(dict(batch=batch,out_of_memory=True,within_reserve=False))
                break
        eligible=[r for r in rows if r['within_reserve']]
        if not eligible:raise ValueError('No measured batch fits; choose a smaller profile or CPU')
        best=max(eligible,key=lambda r:r['samples_per_second'])
        return dict(profile=profile,backend=backend,device=name,context=context,family=family,input_features=len(inputs),measurements=rows,
            batch_size=best['batch'],memory_budget=budget,gameplay_qualified=False,
            purpose='synthetic optimizer resource calibration; does not measure gameplay quality')
    finally:
        optimizer.zero_grad(set_to_none=True)
        del optimizer,model
        gc.collect()
        if device.type=='cuda':torch.cuda.empty_cache()
