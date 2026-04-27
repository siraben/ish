import compileall
import pathlib
import shutil
import sysconfig

src = pathlib.Path(sysconfig.get_path("stdlib")) / "email"
dst = pathlib.Path("/tmp/bench-hot-paths/email-copy")
if dst.exists():
    shutil.rmtree(dst)
shutil.copytree(src, dst)
ok = compileall.compile_dir(str(dst), quiet=1, force=True)
print("ok" if ok else "fail")
