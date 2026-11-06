import re, sys

# Device-type categories. Order matters: first match wins.
# cat id -> (short label shown on screen)
CATS = [
    ("net",    "Network"),   # routers/APs/switches
    ("apple",  "Apple"),
    ("samsung","Samsung"),
    ("google", "Google"),
    ("amazon", "Amazon"),
    ("iot",    "IoT"),
    ("cam",    "Camera"),
    ("printer","Printer"),
    ("tv",     "TV/Media"),
    ("audio",  "Audio"),
    ("pc",     "Computer"),
    ("phone",  "Phone"),
    ("auto",   "Vehicle"),
    ("other",  ""),
]
CAT_ID = {name:i for i,(name,_) in enumerate(CATS)}

# keyword -> category. Checked against the long vendor name, lowercased.
RULES = [
    ("apple", "apple"),
    ("samsung", "samsung"),
    (("google","nest labs","fitbit"), "google"),
    ("amazon", "amazon"),
    (("cisco","meraki","aruba","ubiquiti","netgear","tp-link","tplink","d-link",
      "zyxel","ruckus","juniper","mikrotik","fortinet","arris","ruijie","h3c",
      "extreme networks","cambium","engenius","eero","edgecore","sonicwall",
      "huawei tech","aerohive","watchguard","peplink","draytek","openwrt",
      "technicolor","sagemcom","actiontec","commscope","calix","adtran"), "net"),
    (("hikvision","dahua","axis commun","reolink","wyze","amcrest","lorex",
      "ubnt","uniview","vivotek","swann","annke","foscam","ring","arlo",
      "flir","mobotix"), "cam"),
    (("espressif","tuya","shenzhen","sonoff","itead","shelly","tasmota",
      "broadlink","xiaomi","yeelight","lifx","philips lighting","signify",
      "ikea of sweden","sengled","wiz","govee","particle","texas instrum",
      "nordic semi","silicon lab","realtek semi","mediatek","ampak",
      "azurewave","murata","gainspan","electric imp","smartthings","wink",
      "ecobee","honeywell","emerson","leviton","lutron","rachio","roomba",
      "irobot","august home","schlage","yale","chamberlain","myq"), "iot"),
    (("hewlett","hp inc","brother","canon","epson","lexmark","kyocera",
      "ricoh","xerox","zebra tech","seiko epson","konica"), "printer"),
    (("roku","amazon tech","tivo","nvidia","vizio","sony inter","nintendo",
      "lg electronics","hisense","tcl","skyworth","chromecast","sling",
      "harmonix","sceptre"), "tv"),
    (("sonos","bose","sonance","denon","yamaha","harman","sonance","logitech",
      "marshall","jbl","bang & olufsen","libratone","polk audio"), "audio"),
    (("intel","dell","lenovo","asustek","micro-star","msi","gigabyte",
      "asrock","toshiba","fujitsu","acer","clevo","framework","system76",
      "raspberry pi","microsoft","vmware","parallels","supermicro",
      "quanta","compal","wistron","pegatron","liteon"), "pc"),
    (("oneplus","oppo","vivo mobile","realme","motorola mobility","nokia",
      "htc","zte","lg innotek","sony mobile","transsion","fairphone",
      "google pixel"), "phone"),
    (("tesla","ford motor","general motors","toyota","volkswagen","bmw",
      "continental auto","harman becker","hyundai","rivian","lucid",
      "bosch car","denso"), "auto"),
]

def categorize(long_name):
    n = long_name.lower()
    for keys, cat in RULES:
        if isinstance(keys, str): keys = (keys,)
        for k in keys:
            if k in n:
                return CAT_ID[cat]
    return CAT_ID["other"]

entries = {}  # oui_int -> (short_name, cat)
with open("manuf", encoding="utf-8", errors="replace") as f:
    for line in f:
        if line.startswith("#"): continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 2: continue
        oui = parts[0].strip()
        m = re.match(r'^([0-9A-Fa-f]{2}):([0-9A-Fa-f]{2}):([0-9A-Fa-f]{2})$', oui)
        if not m: continue  # skip /28 /36 MA-M/MA-S long prefixes
        short = parts[1].strip()
        long_name = parts[2].strip() if len(parts) >= 3 and parts[2].strip() else short
        cat = categorize(long_name)
        # prefer a concise display name: short col, trimmed to 20
        name = short[:20]
        key = (int(m.group(1),16)<<16) | (int(m.group(2),16)<<8) | int(m.group(3),16)
        entries[key] = (name, cat)

keys = sorted(entries)
blob = bytearray()
offs = []
names_seen = {}
for k in keys:
    name, cat = entries[k]
    if name in names_seen:
        off = names_seen[name]
    else:
        off = len(blob)
        names_seen[name] = off
        blob += name.encode("ascii", "replace") + b"\x00"
    offs.append((off, cat))

print(f"OUIs: {len(keys)}  blob: {len(blob)} bytes  uniq names: {len(names_seen)}", file=sys.stderr)
print(f"est flash: keys {len(keys)*4} + offs {len(keys)*4} + blob {len(blob)} = {len(keys)*8+len(blob)} bytes", file=sys.stderr)

out = open("oui_vendors.h","w")
w = out.write
w("// Auto-generated from Wireshark 'manuf' (IEEE OUI registry). Do not edit.\n")
w("// Vendor + device-type lookup for the T-Display-S3 UI. Flash-resident.\n")
w("#pragma once\n#include <stdint.h>\n#include <pgmspace.h>\n\n")
w(f"#define OUI_DB_COUNT {len(keys)}\n\n")
# categories
w("enum OuiCat {\n")
for name,_ in CATS: w(f"  OUICAT_{name.upper()},\n")
w("};\n")
w(f"static const char* const OUI_CAT_LABEL[] PROGMEM = {{ ")
w(", ".join('"%s"'%lbl for _,lbl in CATS))
w(" };\n\n")
# keys: 24-bit OUI in low bits
w("static const uint32_t OUI_DB_KEY[OUI_DB_COUNT] PROGMEM = {\n")
for i in range(0,len(keys),12):
    w("  "+",".join("0x%06x"%k for k in keys[i:i+12])+",\n")
w("};\n\n")
# packed: cat in top 8 bits, name offset in low 24
w("static const uint32_t OUI_DB_VAL[OUI_DB_COUNT] PROGMEM = {\n")
packed = [ (cat<<24)|off for (off,cat) in offs ]
for i in range(0,len(packed),12):
    w("  "+",".join("0x%08x"%v for v in packed[i:i+12])+",\n")
w("};\n\n")
# blob
w(f"static const char OUI_DB_NAMES[{len(blob)}] PROGMEM = {{\n")
for i in range(0,len(blob),20):
    w("  "+",".join(str(b) for b in blob[i:i+20])+",\n")
w("};\n")
out.close()
print("wrote oui_vendors.h", file=sys.stderr)
