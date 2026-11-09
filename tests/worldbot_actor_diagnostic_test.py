import importlib.util
from pathlib import Path
p=Path(__file__).resolve().parents[1]/"tools/world-runtime/world_actor_diagnostic.py"
spec=importlib.util.spec_from_file_location("actor_diag",p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
a={"char_guid":93,"level":60,"spent_talent_points":51,"free_talent_points":0,"map":0,"position":[3,4,0],"identity":{"account_id":7,"created_at":123}}
report=m.diagnose({"profiles":[a],"counts":{"online":1}},{"profiles":[{"char_guid":93,"account_id":7,"created_at":123}]},110,100,[0,0,0,0])
r=report["actors"][0];assert r["verification"]=={"runtime_identity":True,"talents51":True,"visible_to_observer":None};assert r["observer_distance_3d"]==5;assert r["snapshot_age_seconds"]==10
b=dict(a);b.pop("identity");b.pop("spent_talent_points");r=m.diagnose({"profiles":[b],"counts":{}},{"profiles":[]},110,100)["actors"][0];assert r["verification"]["runtime_identity"] is None and not r["verification"]["talents51"] and r["verification"]["visible_to_observer"] is None
assert "identity_matches_config" not in a
print("PASS readonly actor diagnostic: identity, missing snapshot data, 51/free points, distance is not visibility, input unmodified")
