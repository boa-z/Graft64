import json
from pathlib import Path

fixture = json.loads(Path(__file__).with_name("report_fixture.json").read_text())
required = {"schema_version", "app_version", "build_commit", "timestamp_utc", "device", "environment", "probes"}
assert required <= fixture.keys()
assert fixture["schema_version"] == 1
assert fixture["device"]["page_size"] > 0
assert isinstance(fixture["probes"], list)
print("JSON report schema fixture passed")
