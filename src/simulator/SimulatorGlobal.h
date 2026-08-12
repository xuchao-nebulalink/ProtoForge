#pragma once

#include <QtGlobal>

#if defined(HWSIM_STATIC_BUILD)
#  define HWSIM_SIMULATOR_API
#elif defined(HWSIM_SIMULATOR_BUILD)
#  define HWSIM_SIMULATOR_API Q_DECL_EXPORT
#else
#  define HWSIM_SIMULATOR_API Q_DECL_IMPORT
#endif
