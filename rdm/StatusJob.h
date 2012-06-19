#ifndef StatusJob_h
#define StatusJob_h

#include <QObject>
#include <QRunnable>
#include <ByteArray.h>
#include <QList>
#include "Job.h"

class StatusJob : public Job
{
    Q_OBJECT
public:
    StatusJob(int i, const ByteArray &query);
    static const char *delimiter;
protected:
    virtual void execute();
private:
    const ByteArray query;
};

#endif
