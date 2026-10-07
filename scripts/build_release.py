"""Build versioned release assets from a qualified package; never publish credentials or source-private data."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def notes():
    history = json.loads((ROOT/'docs/changelog.json').read_text(encoding='utf-8'))
    version = ET.parse(ROOT/'src/BotTrainingStudio/BotTrainingStudio.csproj').findtext('./PropertyGroup/Version')
    if history[0]['version'] != version:
        raise ValueError('Change notes/application versions differ')
    for language in ('ru', 'en'):
        text = '# Bot Training Studio\n\n' + '\n\n'.join('## '+entry['version']+' — '+entry['date']+'\n\n'+
            '\n'.join('- '+line for line in entry[language]) for entry in history)+'\n'
        (ROOT/'docs'/f'CHANGELOG.{language}.md').write_text(text, encoding='utf-8')
    return version


def make_zip(path, package, files):
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as archive:
        for relative in sorted(files):
            source = package/relative
            if source.is_symlink():
                raise ValueError('Package contains a link')
            archive.write(source, relative)


def asset(path):
    if path.stat().st_size >= 2*1024**3:
        raise ValueError('GitHub release asset exceeds 2 GiB: '+path.name)
    return dict(name=path.name, size=path.stat().st_size, sha256=sha(path))


def build(package, output, rar, libraries_only=False):
    package, output = package.resolve(), output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    version = notes()
    previous = output/'release-assets.json'
    if previous.is_file() and json.loads(previous.read_text())['version'] != version:
        raise ValueError('Keep only the current local release: remove verified published previous assets before packaging a new version')
    if json.loads((package/'build.json').read_text(encoding='utf-8-sig'))['version'] != version:
        raise ValueError('Rebuild the application package at the current version')
    python_lock = json.loads((ROOT/'packaging/runtime-cpu-win-x64.lock.json').read_text())
    python_archive = package/'worker/runtime'/python_lock['python_url'].rsplit('/',1)[-1]
    if not python_archive.is_file() or sha(python_archive) != python_lock['python_sha256']:
        raise ValueError('Complete packages require the pinned official Python archive')
    forbidden = {'.dm2','.mvd2','.bsp','.cfg','.safetensors','.npz','.parquet','.pdb','.pyc'}
    top_files = {'BotTrainingStudio.exe','README.md','LICENSE','build.json','libSkiaSharp.dll','libHarfBuzzSharp.dll'}
    factory = json.loads((ROOT/'packaging/factory-models.lock.json').read_text())
    factory_files={'catalog.json':sha(ROOT/'packaging/factory-models.lock.json')}
    for entry in factory['models']:
        factory_files[entry['id']+'/model.json']=entry['metadata_sha256']
        factory_files[entry['id']+'/weights.safetensors']=entry['weights_sha256']
    mirrors=[]
    app, libraries = {}, {}
    for path in sorted(package.rglob('*')):
        if not path.is_file(): continue
        relative = path.relative_to(package).as_posix()
        if '__pycache__' in path.parts or path.suffix == '.pyc': continue
        if path.is_symlink(): raise ValueError('Link in package: '+relative)
        if relative.startswith('libraries/'):
            libraries[relative] = sha(path)
        elif relative.startswith(('worker/factory-models/','Models/')):
            tail=relative.split('/',2)[2] if relative.startswith('worker/') else relative.split('/',1)[1]
            if factory_files.get(tail)!=sha(path):raise ValueError('Unexpected factory file: '+relative)
            if relative.startswith('Models/'):mirrors.append(relative)
            else:app[relative]=sha(path)
        elif relative.startswith(('docs/','worker/')) or relative in top_files:
            if path.suffix in forbidden: raise ValueError('Unexpected private/generated file: '+relative)
            app[relative] = sha(path)
        elif relative != 'package-files.json':
            raise ValueError('Unexpected root file in release package: '+relative)
    library_hash = libraries['libraries/studio-libraries.json']
    inventory = dict(version=version, librarySha256=library_hash, appFiles=app, libraryFiles=libraries)
    (package/'package-files.json').write_text(json.dumps(inventory,indent=2)+'\n',encoding='utf-8')
    receipt = output/'library-assets.json'
    existing = json.loads(receipt.read_text()) if receipt.exists() else {}
    if existing.get('librarySha256') == library_hash and all((output/p['name']).is_file() and sha(output/p['name']) == p['sha256'] for p in existing.get('parts',[])) and existing.get('parts'):
        parts = existing['parts']
    else:
        archive_path=output/'libraries-building.zip'
        print('Compressing packaged libraries',flush=True)
        make_zip(archive_path,package,libraries)
        parts=[]
        with archive_path.open('rb') as source:
            index=1
            while True:
                first=source.read(1024*1024)
                if not first:break
                part=output/f'BotTrainingStudio-libraries-{library_hash[:12]}-win-x64.zip.{index:03d}'
                with part.open('wb') as dest:
                    dest.write(first); remaining=1024**3-len(first)
                    while remaining:
                        block=source.read(min(1024*1024,remaining))
                        if not block:break
                        dest.write(block);remaining-=len(block)
                parts.append(asset(part));index+=1
        # This exact build-owned intermediate is no longer needed after all parts are hashed.
        archive_path.unlink()
        receipt.write_text(json.dumps(dict(librarySha256=library_hash,parts=parts),indent=2),encoding='utf-8')
    if libraries_only:
        print(json.dumps(dict(libraries=parts)));return
    core=output/f'BotTrainingStudio-{version}-app-win-x64.zip'
    make_zip(core,package,[*app,'package-files.json'])
    manifest=dict(schema=1,version=version,librarySha256=library_hash,app=[asset(core)],libraries=parts)
    (output/'BotTrainingStudio-update-win-x64.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    for language in ('RU','EN'):
        shutil.copyfile(package/'docs'/f'Bot_Training_Studio_User_Guide_{language}.docx',output/f'Bot_Training_Studio_User_Guide_{language}.docx')
    # Full offline package: all volumes are <2 GiB and include the same libraries.
    listing=output/'full-package.lst'
    if len(mirrors)!=len(factory_files):raise ValueError('Incomplete Models folder')
    listing.write_text('\n'.join([*app,*libraries,*mirrors,'package-files.json'])+'\n',encoding='utf-16')
    archive=output/f'BotTrainingStudio-{version}-win-x64.rar'
    print('Creating full portable RAR volumes',flush=True)
    subprocess.run([str(rar),'a','-cfg-','-ma5','-m1','-md32m','-mt4','-v1000m','-scul','-y','-idq',str(archive),'@'+str(listing)],cwd=package,check=True)
    volumes=sorted(output.glob(f'BotTrainingStudio-{version}-win-x64*.rar'))
    if not volumes:raise ValueError('No complete package volumes produced')
    subprocess.run([str(rar),'t','-cfg-','-idq',str(volumes[0])],check=True)
    publish=[core,output/'BotTrainingStudio-update-win-x64.json',*[output/p['name'] for p in parts],*volumes,
        *[output/f'Bot_Training_Studio_User_Guide_{lang}.docx' for lang in ('RU','EN')]]
    assets=[asset(path) for path in publish]
    (output/'SHA256SUMS').write_text(''.join(a['sha256']+'  '+a['name']+'\n' for a in assets),encoding='utf-8')
    (output/'release-assets.json').write_text(json.dumps(dict(version=version,assets=assets+[asset(output/'SHA256SUMS')]),indent=2),encoding='utf-8')
    print(json.dumps(dict(version=version,assets=len(assets)+1,bytes=sum(a['size'] for a in assets))),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--package',type=Path,default=ROOT/'dist/Release')
    parser.add_argument('--out',type=Path,default=ROOT/'dist/packages')
    parser.add_argument('--rar',type=Path)
    parser.add_argument('--notes-only',action='store_true')
    parser.add_argument('--libraries-only',action='store_true')
    args=parser.parse_args()
    if args.notes_only: print(notes())
    else: build(args.package,args.out,args.rar,args.libraries_only)
