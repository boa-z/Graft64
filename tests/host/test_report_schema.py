import json
from copy import deepcopy
from pathlib import Path


class ValidationError(AssertionError):
    pass


def validate(instance, schema, path="$"):
    expected_type = schema.get("type")
    type_matches = {
        "object": lambda value: isinstance(value, dict),
        "array": lambda value: isinstance(value, list),
        "string": lambda value: isinstance(value, str),
        "integer": lambda value: isinstance(value, int) and not isinstance(value, bool),
        "boolean": lambda value: isinstance(value, bool),
    }
    if expected_type and not type_matches[expected_type](instance):
        raise ValidationError(f"{path}: expected {expected_type}")
    if "const" in schema and instance != schema["const"]:
        raise ValidationError(f"{path}: expected constant {schema['const']!r}")
    if "enum" in schema and instance not in schema["enum"]:
        raise ValidationError(f"{path}: value is not in enum")
    if "minimum" in schema and instance < schema["minimum"]:
        raise ValidationError(f"{path}: value is below minimum")
    if "minLength" in schema and len(instance) < schema["minLength"]:
        raise ValidationError(f"{path}: string is too short")
    if isinstance(instance, dict):
        required = set(schema.get("required", []))
        missing = required - instance.keys()
        if missing:
            raise ValidationError(f"{path}: missing {sorted(missing)}")
        properties = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            unexpected = instance.keys() - properties.keys()
            if unexpected:
                raise ValidationError(f"{path}: unexpected {sorted(unexpected)}")
        for name, value in instance.items():
            if name in properties:
                validate(value, properties[name], f"{path}.{name}")
    if isinstance(instance, list) and "items" in schema:
        for index, value in enumerate(instance):
            validate(value, schema["items"], f"{path}[{index}]")


root = Path(__file__).parents[2]
schema = json.loads((root / "docs" / "report.schema.json").read_text(encoding="utf-8"))
fixture = json.loads(Path(__file__).with_name("report_fixture.json").read_text(encoding="utf-8"))
validate(fixture, schema)

invalid_status = deepcopy(fixture)
invalid_status["probes"][0]["status"] = "UNKNOWN"
try:
    validate(invalid_status, schema)
except ValidationError:
    pass
else:
    raise AssertionError("probe status enum was not enforced")

missing_nested_field = deepcopy(fixture)
del missing_nested_field["probes"][0]["os_error"]
try:
    validate(missing_nested_field, schema)
except ValidationError:
    pass
else:
    raise AssertionError("nested probe fields were not enforced")

print("JSON report schema fixture and negative cases passed")
