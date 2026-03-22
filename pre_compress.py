Import("env")
import gzip
import os

def compress_html(source, target, env):
    data_dir = os.path.join(env.subst("$PROJECT_DIR"), "data")
    for f in os.listdir(data_dir):
        if f.endswith(".html"):
            src = os.path.join(data_dir, f)
            dst = src + ".gz"
            with open(src, "rb") as fin:
                with gzip.open(dst, "wb", compresslevel=9) as fout:
                    fout.write(fin.read())
            print(f"Compressed {f} -> {f}.gz")

env.AddPreAction("$BUILD_DIR/littlefs.bin", compress_html)
