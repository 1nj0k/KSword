/* Standalone entry point. GUI and CLI compile the same catalog and engine. */
#include "../../Ksword5.1/Ksword5.1/ArkDriverClient/HvmCommandCatalog.c"
#include "../../Ksword5.1/Ksword5.1/ArkDriverClient/HvmCommandEngine.c"

int wmain(int argc, wchar_t** argv)
{
    return KswordHvmCommandMainWide(argc, argv);
}
