#ifndef PEONYCORE_GLOBAL_H
#define PEONYCORE_GLOBAL_H

#include <QtCore/qglobal.h>

#if defined(PEONYCORE_LIBRARY)
#  define PEONYCORESHARED_EXPORT Q_DECL_EXPORT
#else
#  define PEONYCORESHARED_EXPORT Q_DECL_IMPORT
#endif

#endif // PEONYCORE_GLOBAL_H
