using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using Avalonia.VisualTree;
using System.Diagnostics;
using System.Text.Json;

namespace BotTrainingStudio;

public sealed partial class MainWindow
{
    internal async Task<int> TestUpdatesUi(string folder, string python)
    {
        Directory.CreateDirectory(folder); var checks=new List<string>();
        void Check(bool value,string name) { if(!value) throw new Exception(name); checks.Add(name); }
        async Task Until(Func<bool> predicate) { var timer=Stopwatch.StartNew(); while(!predicate()) { if(timer.Elapsed.TotalSeconds>120) throw new TimeoutException(); await Task.Delay(30); } }
        ComboBox Compute() => this.GetVisualDescendants().OfType<ComboBox>().First(c=>c.Name=="ComputeChoice");
        void Select(string backend) { var box=Compute(); box.SelectedItem=box.ItemsSource!.Cast<ComboBoxItem>().First(o=>(string)o.Tag! == backend); }
        void Capture(string name) { using var image=new RenderTargetBitmap(new Avalonia.PixelSize((int)Bounds.Width,(int)Bounds.Height)); image.Render(this); image.Save(Path.Combine(folder,name+".png"), new PngBitmapEncoderOptions()); }
        try
        {
            S.AutoUpdateCheck=false; S.LastSeenVersion=""; S.Python=python; S.Language="ru"; S.Backend="auto"; S.Save(); App.ApplyLanguage(); Navigate("home");
            await Task.Delay(150);
            Check(Compute().IsVisible,"Compute selector exists before Python check"); Select("cuda");
            int ticks=0; var timer=new DispatcherTimer { Interval=TimeSpan.FromMilliseconds(30) }; timer.Tick+=(_,_)=>ticks++; timer.Start();
            var startup = ShowStartupCheckAsync(RefreshRuntimeAsync);
            await Until(()=>_startupDialog?.IsVisible==true);
            Check(!_startupDialog!.GetVisualDescendants().OfType<ComboBox>().Any(), "Startup modal contains no GPU choices");
            await ShowChangesAsync(); Check(_changesDialog==null,"Change notes cannot overlap startup check");
            await Task.Delay(250); Capture("startup-check-ru");
            await startup; timer.Stop();
            await Task.Delay(100);
            Check(_runtimeProbe?.Ready==true,"Actual packaged Python/libraries probe succeeds");
            Check(ticks>3,"Runtime probe does not block UI events");
            Check(Compute().IsVisible && (string)((ComboBoxItem)Compute().SelectedItem!).Tag! == "cuda","GPU choice remains visible and selected after Python readiness");
            Check(StudioSettings.Load().Backend=="cuda","Computation selection is persisted");
            Check(_startupDialog==null,"Successful probe closes startup modal automatically");
            Check(!this.GetVisualDescendants().OfType<Button>().Any(b=>b.Name=="InstallRuntime"), "Home does not contain a transient Python installer");
            Capture("home-ready-ru");
            var validProbe=_runtimeProbe;
            foreach(string language in new[]{"ru","en"})
            {
                S.Language=language; App.ApplyLanguage(); Navigate("home");
                var blocked=new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                var checking=ShowStartupCheckAsync(async()=>{await blocked.Task; _runtimeProbe=new(false,"Test missing Python");});
                await Until(()=>_startupDialog?.IsVisible==true); await Task.Delay(250); Capture("startup-check-"+language);
                blocked.SetResult();
                await Until(()=>_startupDialog?.IsPrimaryButtonEnabled==true);
                await Task.Delay(250); Capture("startup-missing-"+language);
                string open= L("Открыть настройки", "Open Settings");
                var button=_startupDialog!.GetVisualDescendants().OfType<Button>().Single(b=>b.Content?.ToString()==open);
                Check(button.IsEffectivelyVisible && button.IsEffectivelyEnabled,"Setup action is visible and usable: "+language);
                button.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent)); await checking; await Task.Delay(100);
                Check(_page=="settings" && this.GetVisualDescendants().OfType<Button>().Any(b=>b.Name=="InstallRuntime"),"Missing Python opens setup only on request: "+language);
            }
            _runtimeProbe=validProbe; S.Language="ru"; App.ApplyLanguage(); Navigate("home");
            var continued=ShowStartupCheckAsync(()=>{_runtimeProbe=new(false,"Test missing Python");return Task.CompletedTask;});
            await Until(()=>_startupDialog?.GetVisualDescendants().OfType<Button>().Any(b=>b.Content?.ToString()=="Продолжить без обучения")==true);
            _startupDialog!.GetVisualDescendants().OfType<Button>().Single(b=>b.Content?.ToString()=="Продолжить без обучения")
                .RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
            await continued;
            Check(_page=="home" && !_lifetime.IsCancellationRequested,"Continue without training leaves Studio open on Home");
            _runtimeProbe=validProbe;
            string oldPackage=Path.Combine(folder,"old-package"), newPackage=Path.Combine(folder,"new-package"), custom=Path.Combine(folder,"custom-worker");
            Directory.CreateDirectory(Path.Combine(oldPackage,"worker")); Directory.CreateDirectory(Path.Combine(newPackage,"worker")); Directory.CreateDirectory(custom);
            File.WriteAllText(Path.Combine(oldPackage,"BotTrainingStudio.exe"),""); File.WriteAllText(Path.Combine(oldPackage,"build.json"),"{}");
            Check(StudioSettings.ResolveWorkerDirectory(Path.Combine(oldPackage,"worker"),newPackage)==Path.Combine(newPackage,"worker"),"Moving portable app replaces stale packaged worker path");
            Check(StudioSettings.ResolveWorkerDirectory(custom,newPackage)==custom,"Explicit development worker remains selected");
            S.LastSeenVersion="0.2.0"; S.Save();
            var first=FirstVersionStartAsync(); await Until(()=>_changesDialog?.GetVisualDescendants().OfType<Button>().Any(b=>b.Content?.ToString()=="Продолжить")==true);
            Check(_changesDialog!.Title!.ToString()!.Contains(AppVersion.Current),"First launch of new version displays its change notes");
            await Task.Delay(300); // Let the dialog's entrance animation finish before capture.
            Capture("first-version-notes-ru");
            var continueButton=_changesDialog.GetVisualDescendants().OfType<Button>().Single(b=>b.Content?.ToString()=="Продолжить");
            continueButton.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent)); await first;
            Check(StudioSettings.Load().LastSeenVersion==AppVersion.Current,"Viewed version is saved after acknowledging change notes");
            _versionStarted=false; await FirstVersionStartAsync(); Check(_changesDialog==null,"Same version does not show change notes again");
            var manual=ShowChangesAsync(); await Until(()=>_changesDialog!=null); Check(true,"Change notes can be opened manually");
            _changesDialog!.Hide(); await manual;
            _jobUiActive=true; await InstallUpdateAsync(new("99.0.0","",[]));
            Check(_updateCancellation==null,"An active job blocks update installation"); _jobUiActive=false;
            using(var cancellation=new CancellationTokenSource())
            {
                _updateCancellation=cancellation; Start("hardware");
                Check(!_runner.IsRunning,"Update installation blocks starting a training worker");
                await RequestStopAsync(); Check(cancellation.IsCancellationRequested,"Cancel requests stop update download"); _updateCancellation=null;
            }
            foreach(string language in new[]{"ru","en"})
            {
                S.Language=language; S.Save(); App.ApplyLanguage(); Navigate("settings");
                await Task.Delay(100);
                Check(Compute().IsVisible,"Settings compute selector: "+language);
                Check(this.GetVisualDescendants().OfType<CheckBox>().Any(c=>c.Name=="AutoUpdateCheck"),"Automatic update setting: "+language);
                Capture("settings-"+language); Navigate("home"); await Task.Delay(100); Capture("home-"+language);
            }
            var closingCheck=ShowStartupCheckAsync(async()=>await Task.Delay(30000,_lifetime.Token));
            await Until(()=>_startupDialog?.GetVisualDescendants().OfType<Button>().Any(b=>b.Content?.ToString()==L("Выйти","Quit"))==true);
            var quit=_startupDialog!.GetVisualDescendants().OfType<Button>().Single(b=>b.Content?.ToString()==L("Выйти","Quit"));
            quit.RaiseEvent(new RoutedEventArgs(Avalonia.Controls.Button.ClickEvent));
            await closingCheck.WaitAsync(TimeSpan.FromSeconds(5));
            Check(_lifetime.IsCancellationRequested && _startupDialog==null,"Quit cancels startup check and closes dialog without waiting for completion");
            File.WriteAllText(Path.Combine(folder,"updates-ui-test.json"),JsonSerializer.Serialize(new{pass=true,checks},Updates.Json)); return 0;
        }
        catch(Exception error) { File.WriteAllText(Path.Combine(folder,"updates-ui-test.json"),JsonSerializer.Serialize(new{pass=false,checks,error=error.ToString()},Updates.Json)); return 1; }
    }
}
