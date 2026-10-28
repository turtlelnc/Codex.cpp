// Native browser service; no Python interpreter or package installation.
#include "codex_web_native.inc"
static int launch_web(int argc,char** argv){return native_web::main(argc,argv);}
