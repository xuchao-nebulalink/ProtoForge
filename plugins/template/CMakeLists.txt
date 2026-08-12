# The NAME must match the identifier passed to HWSIM_EXPORT_STATIC_PLUGIN in
# TemplatePlugin.cpp: CMake generates the extern declaration from it when the
# plugin is linked statically.
hwsim_add_protocol_plugin(
    NAME tlv_template
    CLASS_NAME TemplatePlugin
    SOURCES
        TemplateProtocol.h
        TemplateProtocol.cpp
        TemplatePlugin.h
        TemplatePlugin.cpp
        template.json
)
