from pathlib import Path


Import("env")

header = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "TinyGSM" / "src" / "TinyGsmClientBG96.h"
if not header.exists():
    raise RuntimeError(f"TinyGSM BG96 driver not found: {header}")

source = header.read_text(encoding="utf-8")
replacements = {
    '+QSSLCFG=\\"sslversion\\",0,3': '+QSSLCFG=\\"sslversion\\",1,3',
    '+QSSLCFG=\\"ciphersuite\\",0,0X0035': '+QSSLCFG=\\"ciphersuite\\",1,0XFFFF',
    '+QSSLCFG=\\"seclevel\\",0,1': '+QSSLCFG=\\"seclevel\\",1,0',
    '+QSSLCFG=\\"sni\\",0,1': '+QSSLCFG=\\"sni\\",1,1',
}

for old, new in replacements.items():
    if old in source:
        source = source.replace(old, new)

seclevel_line = '      sendAT(GF("+QSSLCFG=\\"seclevel\\",1,0"));'
ignore_time_line = '      sendAT(GF("+QSSLCFG=\\"ignorelocaltime\\",1,1"));'
if ignore_time_line not in source:
    source = source.replace(
        seclevel_line + "\n      if (waitResponse(5000L) != 1) return false;",
        seclevel_line
        + "\n      if (waitResponse(5000L) != 1) return false;\n"
        + ignore_time_line
        + "\n      if (waitResponse(5000L) != 1) return false;",
    )

required = (
    '+QSSLCFG=\\"sslversion\\",1,3',
    '+QSSLCFG=\\"ciphersuite\\",1,0XFFFF',
    '+QSSLCFG=\\"seclevel\\",1,0',
    '+QSSLCFG=\\"sni\\",1,1',
    '+QSSLCFG=\\"ignorelocaltime\\",1,1',
)
if any(value not in source for value in required):
    raise RuntimeError("TinyGSM BG96 SSL context patch could not be applied")

header.write_text(source, encoding="utf-8")
