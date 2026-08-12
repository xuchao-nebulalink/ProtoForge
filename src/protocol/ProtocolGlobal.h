#pragma once

#include <QtGlobal>

#if defined(HWSIM_STATIC_BUILD)
#  define HWSIM_PROTOCOL_API
#elif defined(HWSIM_PROTOCOL_BUILD)
#  define HWSIM_PROTOCOL_API Q_DECL_EXPORT
#else
#  define HWSIM_PROTOCOL_API Q_DECL_IMPORT
#endif
