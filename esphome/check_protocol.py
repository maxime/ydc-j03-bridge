from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
standalone = (ROOT / "src/main.cpp").read_bytes()
esphome = (ROOT / "esphome/dryer_bridge.cpp").read_bytes()


def section(source, start, end):
    offset = source.index(start)
    return source[offset : source.index(end, offset)]


for start, end in (
    (b"static const int kPanelRxPin", b"static const uint32_t kBurstGapMs"),
    (b"const DryerCommand kCommands[]", b"\n};"),
    (b"static const PulseEdge kOnEdges[]", b"sizeof(kTimerEdges[0]);"),
    (b"bool samePrefix(", b"void describeMainChange("),
    (b"void saveOnWave()", b"// Steps the Timer button"),
):
    assert section(esphome, start, end) == section(standalone, start, end), (
        f"Dryer protocol differs at {start.decode()}"
    )

setters = section(standalone, b"// Steps the Timer button", b"void runCommand(").strip()
assert setters in esphome, "Dryer target setters differ"
