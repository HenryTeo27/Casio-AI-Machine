import fs from "node:fs";
import path from "node:path";

const fontsDir = path.join(process.cwd(), "app", "utils", "fonts");
const fontconfigFile = path.join(fontsDir, "fonts.conf");
const fontconfigCacheDir = "/tmp/fontconfig-cache";

try {
    fs.mkdirSync(fontconfigCacheDir, { recursive: true });
} catch {
    // Font rendering can still continue; fontconfig will report if it cannot cache.
}

process.env.FONTCONFIG_FILE ||= fontconfigFile;
process.env.FONTCONFIG_PATH ||= fontsDir;
process.env.XDG_CACHE_HOME ||= "/tmp";
