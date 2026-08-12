#pragma once

#include <QtGlobal>

#if defined(HWSIM_STATIC_BUILD)
#  define HWSIM_SCRIPTING_API
#elif defined(HWSIM_SCRIPTING_BUILD)
#  define HWSIM_SCRIPTING_API Q_DECL_EXPORT
#else
#  define HWSIM_SCRIPTING_API Q_DECL_IMPORT
#endif
