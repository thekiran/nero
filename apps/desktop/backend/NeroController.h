#pragma once

#include <QObject>
#include <QString>

/*
 * NeroController bridges the QML frontend to the shared C11 core. It never
 * reimplements networking or parsing and never shells out to the CLI: it calls
 * the same C functions the `nero` and `neroconsole` frontends use, so all three
 * report identical capability and diagnostic data. Keep Qt types out of the C
 * core; this wrapper is the only Qt-aware layer that touches it.
 */
class NeroController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)

public:
    explicit NeroController(QObject *parent = nullptr);

    QString version() const;

    /* Returns the real read-only diagnostics produced by the core's
     * nero_doctor(), captured verbatim. No fabricated values. */
    Q_INVOKABLE QString doctorReport() const;
};
