# The kernel loader self-test (qzkrnl shell command "ldrtest"): a driver
# and the export driver it imports, both loaded from disk.

spec2def(qzldrdll.sys qzldrdll.spec ADD_IMPORTLIB)
add_library(qzldrdll MODULE qzldrdll.c ${CMAKE_CURRENT_BINARY_DIR}/qzldrdll.def)
set_module_type(qzldrdll kernelmodedriver)
add_importlibs(qzldrdll ntoskrnl)
add_cd_file(TARGET qzldrdll DESTINATION reactos/system32/drivers NO_CAB FOR all)

add_library(qzldrtst MODULE qzldrtst.c)
set_module_type(qzldrtst kernelmodedriver)
add_importlibs(qzldrtst qzldrdll ntoskrnl)
add_cd_file(TARGET qzldrtst DESTINATION reactos/system32/drivers NO_CAB FOR all)
