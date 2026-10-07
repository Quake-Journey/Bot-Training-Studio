using System.Text.Json;

namespace BotTrainingStudio;

/// <summary>Worker output cannot create an unbounded queue of UI callbacks or log text.</summary>
internal sealed class JobEventBuffer
{
    private readonly object _gate = new();
    private readonly Queue<string> _lines = new();
    private JsonElement? _latest;
    public void Receive(JsonElement message)
    {
        string line = message.ToString(); // Serialization stays on the worker thread.
        lock (_gate) { _latest = message; Enqueue(line); }
    }
    public void Log(string text) { lock (_gate) Enqueue(text); }
    private void Enqueue(string text)
    {
        if (_lines.Count >= 80) _lines.Dequeue();
        _lines.Enqueue(text.Length > 2048 ? text[..2048] + "…" : text);
    }
    public (JsonElement? message, string[] lines) Drain()
    {
        lock (_gate)
        {
            var result = (_latest, _lines.ToArray());
            _latest = null; _lines.Clear();
            return result;
        }
    }
}
