include "llvm/Option/OptParser.td"

class F<string name, string help> : Flag<["-"], name>, HelpText<help>;
class FF<string name, string help> : Flag<["--"], name>, HelpText<help>;
class S<string name, string meta, string help>
    : Separate<["-"], name>, HelpText<help>, MetaVarName<meta>;

def help : FF<"help", "Display available options">;
def : F<"h", "Alias for --help">, Alias<help>;

def max_concurrency
    : S<"c", "<ulong>",
        "Maximum number of cache files to scan concurrently. "
        "If 0, use the hardware thread count.">;

def host_interface : S<"i", "<string>", "Host interface to bind to.">;

def port
    : S<"p", "<uint>",
        "Port to listen on. Set to 0 to bind to any available port.">;

def scan_interval
    : S<"t", "<int>",
        "Seconds between periodic rescans of the cache paths for new entries. "
        "Set to 0 to disable periodic rescanning.">;

def min_interval
    : S<"m", "<number>",
        "Minimum seconds between on-demand rescans triggered by a cache miss.">;

def verbose_logging : F<"v", "Enable verbose logging.">;
