# Feeds the secrets from .env into the build as preprocessor macros.
#
# .env is a plain KEY=VALUE file, kept out of the repository:
#
#     SSID=my-network
#     PWD=my-password
#     NEWS_API=0123456789abcdef
#     TOKEN_BOT=123456789:AA...
#     SOCKS5_HOST=proxy.example.net
#     SOCKS5_PORT=1080
#     SOCKS5_USER=someone
#     SOCKS5_PASSWORD=secret
#
# WIFI_SSID / WIFI_PASS are accepted as key names too, as is NEWS_API_KEY for
# the news one and BOT_TOKEN for the Telegram one. Values may be quoted.
# Nothing is read from the process environment: "PWD" there is the shell's
# current directory, which would silently become the Wi-Fi password.
#
# Each macro is independent of the others, because the firmware gates on them
# separately:
#
#   * no SSID means no radio at all,
#   * NEWS_API_KEY missing only costs the world-news screen,
#   * TELEGRAM_BOT_TOKEN or SOCKS5_HOST missing only costs the pager screen,
#     which is deliberately proxy-only -- see socks5.cpp.
#
# With no .env at all the build still succeeds and the clock is set by hand at
# the console.

import os

Import("env")

ENV_FILE = os.path.join(env["PROJECT_DIR"], ".env")

ALIASES = {
    "SSID": "WIFI_SSID",
    "WIFI_SSID": "WIFI_SSID",
    "PWD": "WIFI_PASS",
    "PASS": "WIFI_PASS",
    "PASSWORD": "WIFI_PASS",
    "WIFI_PASS": "WIFI_PASS",
    "NEWS_API": "NEWS_API_KEY",
    "NEWS_API_KEY": "NEWS_API_KEY",
    "NEWSAPI_KEY": "NEWS_API_KEY",
    "TOKEN_BOT": "TELEGRAM_BOT_TOKEN",
    "BOT_TOKEN": "TELEGRAM_BOT_TOKEN",
    "TELEGRAM_TOKEN": "TELEGRAM_BOT_TOKEN",
    "TELEGRAM_BOT_TOKEN": "TELEGRAM_BOT_TOKEN",
    "SOCKS5_HOST": "SOCKS5_HOST",
    "SOCKS5_PORT": "SOCKS5_PORT",
    "SOCKS5_USER": "SOCKS5_USER",
    "SOCKS5_PASSWORD": "SOCKS5_PASS",
    "SOCKS5_PASS": "SOCKS5_PASS",
}

# Printed by name only; the values are secrets.
MACROS = (
    "WIFI_SSID",
    "WIFI_PASS",
    "NEWS_API_KEY",
    "TELEGRAM_BOT_TOKEN",
    "SOCKS5_HOST",
    "SOCKS5_USER",
    "SOCKS5_PASS",
)

# The one value the firmware wants as a number rather than a string literal.
NUMERIC = ("SOCKS5_PORT",)


def read_env_file(path):
    values = {}
    if not os.path.isfile(path):
        return values

    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            key = key.strip().upper()
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            if key in ALIASES and value:
                values[ALIASES[key]] = value
    return values


creds = read_env_file(ENV_FILE)

defines = [
    (name, env.StringifyMacro(creds[name])) for name in MACROS if name in creds
]

for name in NUMERIC:
    if name not in creds:
        continue
    try:
        defines.append((name, int(creds[name], 10)))
    except ValueError:
        print('env_flags: %s="%s" is not a number, ignoring it' % (name, creds[name]))

if defines:
    env.Append(CPPDEFINES=defines)

# The SSID and the proxy host are echoed back so a typo in .env is visible;
# the passwords, the API key and the bot token are only ever reported as
# present or absent.
if "WIFI_SSID" in creds:
    print('env_flags: Wi-Fi SSID "%s" taken from .env' % creds["WIFI_SSID"])
else:
    print("env_flags: no SSID in .env, building without NTP, news or the pager")

if "NEWS_API_KEY" in creds:
    print("env_flags: NewsAPI key taken from .env")
else:
    print("env_flags: no NEWS_API in .env, building without the world-news screen")

if "TELEGRAM_BOT_TOKEN" in creds and "SOCKS5_HOST" in creds:
    print(
        'env_flags: Telegram bot token taken from .env, through SOCKS5 %s:%s%s'
        % (
            creds["SOCKS5_HOST"],
            creds.get("SOCKS5_PORT", "1080 (default)"),
            " (authenticated)" if creds.get("SOCKS5_USER") else "",
        )
    )
elif "TELEGRAM_BOT_TOKEN" in creds:
    print("env_flags: no SOCKS5_HOST in .env, building without the pager screen")
else:
    print("env_flags: no TOKEN_BOT in .env, building without the pager screen")
