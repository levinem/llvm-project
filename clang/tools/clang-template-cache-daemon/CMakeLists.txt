set(LLVM_LINK_COMPONENTS
  Option
  Support
  SupportHTTP
  )

set(LLVM_TARGET_DEFINITIONS Opts.td)
tablegen(LLVM Opts.inc -gen-opt-parser-defs)
add_public_tablegen_target(TemplateCacheDaemonOptsTableGen)

add_clang_tool(clang-template-cache-daemon
  clang-template-cache-daemon.cpp

  DEPENDS
  TemplateCacheDaemonOptsTableGen
  GENERATE_DRIVER
  )
