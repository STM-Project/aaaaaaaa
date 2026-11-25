import os
import pathlib
import re
import secrets
import sys

password_file = pathlib.Path(__file__).resolve().with_name("wifi-password")
try:
    with password_file.open(encoding="ascii") as existing:
        password = existing.read().strip()
except FileNotFoundError:
    password = secrets.token_urlsafe(18)
    with os.fdopen(os.open(password_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600),
                   "w", encoding="ascii") as new:
        new.write(password + "\n")

if not re.fullmatch(r"[A-Za-z0-9_-]{12,63}", password):
    sys.exit("wifi-password: oczekiwano 12-63 znaków A-Z a-z 0-9 _ -")

header = pathlib.Path(sys.argv[1])
contents = f'#define BOOT_WIFI_PASSWORD "{password}"\n'
if not header.exists() or header.read_text(encoding="ascii") != contents:
    with os.fdopen(os.open(header, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600),
                   "w", encoding="ascii") as generated:
        generated.write(contents)
