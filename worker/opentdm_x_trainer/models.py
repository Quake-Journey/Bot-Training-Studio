"""Versioned candidate models; profile capacity is not a gameplay quality claim."""
from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class Profile:
    name: str
    width: int
    layers: int
    heads: int
    context: int
    recommended_free_gib: int


PROFILES = {
    "reference": Profile("reference", 64, 2, 0, 1, 0),
    "compact": Profile("compact", 128, 2, 4, 16, 3),
    "balanced": Profile("balanced", 256, 4, 8, 32, 8),
    "large": Profile("large", 512, 6, 8, 64, 18),
    "xl": Profile("xl", 768, 8, 12, 128, 25),
}


def catalog():
    return [dict(asdict(p), gameplay_qualified=False,
                 purpose="motion research; not a full gameplay teacher") for p in PROFILES.values()]


def choose_profile(free_gib, backend):
    if backend == "cpu":
        return "compact"
    for key in ("xl", "large", "balanced", "compact"):
        if free_gib >= PROFILES[key].recommended_free_gib:
            return key
    return "reference"


def create(profile, input_features, outputs=3):
    import torch
    from torch import nn
    p = PROFILES[profile]
    if profile == "reference":
        # Same state dictionary layout as the initial separately archived prototype.
        return nn.Sequential(nn.Linear(input_features, 64), nn.Tanh(),
                             nn.Linear(64, 64), nn.Tanh(), nn.Linear(64, outputs))

    class TemporalTeacher(nn.Module):
        def __init__(self):
            super().__init__()
            self.input = nn.Linear(input_features, p.width)
            self.position = nn.Parameter(torch.randn(1, p.context, p.width) * .01)
            layer = nn.TransformerEncoderLayer(p.width, p.heads, p.width * 4,
                        dropout=0., batch_first=True, activation="gelu")
            self.encoder = nn.TransformerEncoder(layer, p.layers, enable_nested_tensor=False)
            self.output = nn.Sequential(nn.LayerNorm(p.width), nn.Linear(p.width, outputs))
            self.activation_checkpointing = False

        def forward(self, x):
            # Single-frame datasets are allowed for pipeline checks, but the UI
            # and manifest preserve their actual context=1; never fake history.
            if x.dim() == 2:
                x = x.unsqueeze(1)
            if x.size(1) > p.context:
                raise ValueError("Dataset context exceeds this profile; reprepare explicitly")
            y = self.input(x) + self.position[:, :x.size(1)]
            # All tokens precede the predicted future: bidirectional attention
            # over the observed past is permitted; no future label enters here.
            if self.training and self.activation_checkpointing:
                from torch.utils.checkpoint import checkpoint
                for layer in self.encoder.layers:
                    y = checkpoint(layer, y, use_reentrant=False)
                if self.encoder.norm is not None:
                    y = self.encoder.norm(y)
            else:
                y = self.encoder(y)
            return self.output(y[:, -1])

    return TemporalTeacher()
