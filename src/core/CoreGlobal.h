#pragma once

#include <QtGlobal>

#if defined(HWSIM_STATIC_BUILD)
#  define HWSIM_CORE_API
#elif defined(HWSIM_CORE_BUILD)
#  define HWSIM_CORE_API Q_DECL_EXPORT
#else
#  define HWSIM_CORE_API Q_DECL_IMPORT
#endif

#if defined(_MSC_VER)
#  define HWSIM_FUNCTION __FUNCSIG__
#else
#  define HWSIM_FUNCTION __PRETTY_FUNCTION__
#endif
