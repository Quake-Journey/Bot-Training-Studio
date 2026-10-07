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
            await RefreshRuntimeAsync(); timer.Stop();
            await Task.Delay(100);
            Check(_runtimeProbe?.Ready==true,"Actual packaged Python/libraries probe succeeds");
            Check(ticks>3,"Runtime probe does not block UI events");
            Check(Compute().IsVisible && (string)((ComboBoxItem)Compute().SelectedItem!).Tag! == "cuda","GPU choice remains visible and selected after Python readiness");
            Check(StudioSettings.Load().Backend=="cuda","Computation selection is persisted");
            Capture("home-ready-ru");
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
            File.WriteAllText(Path.Combine(folder,"updates-ui-test.json"),JsonSerializer.Serialize(new{pass=true,checks},Updates.Json)); return 0;
        }
        catch(Exception error) { File.WriteAllText(Path.Combine(folder,"updates-ui-test.json"),JsonSerializer.Serialize(new{pass=false,checks,error=error.ToString()},Updates.Json)); return 1; }
    }
}
