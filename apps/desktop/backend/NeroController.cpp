#include "NeroController.h"

#include <cstdio>
#include <cstdlib>

extern "C" {
#include "nero/doctor.h"
}

#ifndef NERO_BUILD_VERSION
#define NERO_BUILD_VERSION "development"
#endif

NeroController::NeroController(QObject *parent) : QObject(parent) {}

QString NeroController::version() const
{
    return QString::fromUtf8(NERO_BUILD_VERSION);
}

QString NeroController::doctorReport() const
{
#if defined(_WIN32)
    /* open_memstream is POSIX-only; capture through a temp file on Windows. */
    FILE *stream = tmpfile();
    if (stream == nullptr)
        return QStringLiteral("doctor diagnostics unavailable");
    nero_doctor(stream);
    fflush(stream);
    fseek(stream, 0, SEEK_END);
    long size = ftell(stream);
    QString result;
    if (size > 0) {
        rewind(stream);
        QByteArray bytes(static_cast<int>(size), Qt::Uninitialized);
        size_t read = fread(bytes.data(), 1, static_cast<size_t>(size), stream);
        result = QString::fromUtf8(bytes.constData(), static_cast<int>(read));
    }
    fclose(stream);
    return result;
#else
    char *buffer = nullptr;
    size_t length = 0;
    FILE *stream = open_memstream(&buffer, &length);
    if (stream == nullptr)
        return QStringLiteral("doctor diagnostics unavailable");
    nero_doctor(stream);
    fclose(stream); /* flushes and finalizes buffer/length */
    QString result = QString::fromUtf8(buffer ? buffer : "", static_cast<int>(length));
    free(buffer);
    return result;
#endif
}
