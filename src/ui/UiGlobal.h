#pragma once

#include <QtGlobal>

#if defined(HWSIM_STATIC_BUILD)
#  define HWSIM_UI_API
#elif defined(HWSIM_UI_BUILD)
#  define HWSIM_UI_API Q_DECL_EXPORT
#else
#  define HWSIM_UI_API Q_DECL_IMPORT
#endif
