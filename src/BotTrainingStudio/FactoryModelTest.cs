using System.Text.Json;

namespace BotTrainingStudio;

internal static class FactoryModelTest
{
    internal static int Run(string folder)
    {
        Directory.CreateDirectory(folder);var checks=new List<string>();
        void Check(bool value,string label) { if(!value)throw new Exception(label);checks.Add(label); }
        try
        {
            string package=Path.Combine(folder,"old-updater-package"), source=Path.Combine(AppContext.BaseDirectory,"worker","factory-models");
            string staged=Path.Combine(package,"worker","factory-models");
            foreach(var file in Directory.EnumerateFiles(source,"*",SearchOption.AllDirectories))
            {
                string dest=Path.Combine(staged,Path.GetRelativePath(source,file));Directory.CreateDirectory(Path.GetDirectoryName(dest)!);File.Copy(file,dest,true);
            }
            string user=Path.Combine(folder,"UserModels","custom-model");Directory.CreateDirectory(user);
            File.WriteAllText(Path.Combine(user,"user-data.txt"),"previous learning");
            Check(!Directory.Exists(Path.Combine(package,"Models")),"Previous updater payload has no root Models yet");
            FactoryModels.Ensure(package);
            using var catalog=JsonDocument.Parse(File.ReadAllText(Path.Combine(package,"Models","catalog.json")));
            var entries=catalog.RootElement.GetProperty("models").EnumerateArray().ToArray();
            Check(entries.Length==2,"Factory materializes from previous updater compatible worker payload");
            foreach(var entry in entries)
                Check(File.ReadAllBytes(Path.Combine(package,"Models",entry.GetProperty("id").GetString()!,"weights.safetensors"))
                    .SequenceEqual(File.ReadAllBytes(Path.Combine(source,entry.GetProperty("id").GetString()!,"weights.safetensors"))),"Factory weights are byte-identical");
            string pointer=Path.Combine(package,"Models","catalog.json");var timestamp=File.GetLastWriteTimeUtc(pointer);
            FactoryModels.Ensure(package);
            Check(File.GetLastWriteTimeUtc(pointer)==timestamp,"Unchanged catalog is not rewritten on startup");
            Check(File.ReadAllText(Path.Combine(user,"user-data.txt"))=="previous learning","Existing user training is preserved");
            string corrupt=Path.Combine(package,"Models",entries[0].GetProperty("id").GetString()!,"weights.safetensors");
            File.WriteAllText(corrupt,"user modification");
            bool rejected=false;try { FactoryModels.Ensure(package); } catch(InvalidDataException) { rejected=true; }
            Check(rejected && File.ReadAllText(corrupt)=="user modification","Modified model is reported without silent overwrite");
            File.WriteAllText(Path.Combine(folder,"factory-storage-test.json"),JsonSerializer.Serialize(new{pass=true,checks},Updates.Json));return 0;
        }
        catch(Exception error) { File.WriteAllText(Path.Combine(folder,"factory-storage-test.json"),JsonSerializer.Serialize(new{pass=false,checks,error=error.ToString()},Updates.Json));return 1; }
    }
}
