#!/usr/bin/env python3
"""Run native wire regressions using headers/libs from an existing CMake build.
Usage: python3 tests/run_spline_packet_regression.py /path/to/existing-build
Does not start the server or touch its data/configuration.
"""
import pathlib, shlex, subprocess, sys, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
build=pathlib.Path(sys.argv[1]).resolve()
flags=(build/"src/game/CMakeFiles/game.dir/flags.make").read_text().splitlines()
includes=next(x.split(" = ",1)[1] for x in flags if x.startswith("CXX_INCLUDES ="))
# Point source includes at this checkout while retaining generated build includes.
old=next(x[2:-9] for x in shlex.split(includes) if x.endswith("/src/game"))
includes=includes.replace(old,str(root))
with tempfile.TemporaryDirectory(prefix="spline-packet-") as tmp:
    exe=str(pathlib.Path(tmp)/"test")
    sources=[root/"tests/spline_packet_regression.cpp"]+[root/"src/game/Movement/spline"/f for f in ["packet_builder.cpp","MoveSpline.cpp","spline.cpp","util.cpp"]]
    # Garbage collection omits the World-dependent config wrapper; production serializer is used directly.
    cmd=["c++","-std=c++14","-O1","-DNDEBUG","-ffunction-sections","-fdata-sections","-DDO_MYSQL"]+shlex.split(includes)+[str(x) for x in sources]+["-Wl,--gc-sections",str(build/"src/shared/libshared.a"),str(build/"src/framework/libframework.a"),str(build/"dep/g3dlite/libg3dlite.a"),str(build/"dep/cpptrace/libcpptrace.a"),str(build/"_deps/libdwarf-build/src/lib/libdwarf/libdwarf.a"),str(build/"_deps/zstd-build/lib/libzstd.a"),"-lssl","-lcrypto","-lz","-lpthread","-ldl","-o",exe]
    subprocess.run(cmd,check=True)
    subprocess.run([exe],check=True)
