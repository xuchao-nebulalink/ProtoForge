#pragma once

#include <QtGlobal>

#if defined(HWSIM_STATIC_BUILD)
#  define HWSIM_TRANSPORT_API
#elif defined(HWSIM_TRANSPORT_BUILD)
#  define HWSIM_TRANSPORT_API Q_DECL_EXPORT
#else
#  define HWSIM_TRANSPORT_API Q_DECL_IMPORT
#endif
