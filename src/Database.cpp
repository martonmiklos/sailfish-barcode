/*
The MIT License (MIT)

Copyright (c) 2018 Slava Monich

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "Database.h"
#include "HistoryModel.h"
#include "Settings.h"

#include "HarbourDebug.h"

#include <QQmlEngine>
#include <QCryptographicHash>
#include <QDir>

#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

#define SETTINGS_TABLE          "settings"
#define SETTINGS_FIELD_KEY      "key"
#define SETTINGS_FIELD_VALUE    "value"

#define HISTORY_TMP_TABLE        HISTORY_TABLE "_tmp"

// ==========================================================================
// Database::Private
// ==========================================================================

class Database::Private {
public:
    static const QString DB_TYPE;
    static const QString DB_NAME;

    static QString gDatabasePath;
    static QDir gImageDir;

    typedef void (Settings::*SetBool)(bool aValue);
    typedef void (Settings::*SetInt)(int aValue);
    typedef void (Settings::*SetString)(QString aValue);

    static QVariant settingsValue(QSqlDatabase aDb, QString aKey);
    static void migrateBool(QSqlDatabase aDb, QString aKey,
        Settings* aSettings, SetBool aSetter);
    static void migrateInt(QSqlDatabase aDb, QString aKey,
        Settings* aSettings, SetInt aSetter);
    static void migrateString(QSqlDatabase aDb, QString aKey,
        Settings* aSettings, SetString aSetter);
};

const QString Database::Private::DB_TYPE("QSQLITE");
const QString Database::Private::DB_NAME("CodeReader");

QString Database::Private::gDatabasePath;
QDir Database::Private::gImageDir;

QVariant Database::Private::settingsValue(QSqlDatabase aDb, QString aKey)
{
    QSqlQuery query(aDb);
    query.prepare("SELECT value FROM settings WHERE key = ?");
    query.addBindValue(aKey);
    if (query.exec()) {
        if (query.next()) {
            QVariant result = query.value(0);
            if (result.isValid()) {
                HDEBUG(aKey << result);
                return result;
            }
        } else {
            HWARN(aKey << query.lastError());
        }
    } else {
        HWARN(aKey << query.lastError());
    }
    return QVariant();
}

void Database::Private::migrateBool(QSqlDatabase aDb, QString aKey,
    Settings* aSettings, SetBool aSetter)
{
    QVariant value = settingsValue(aDb, aKey);
    if (value.isValid()) {
        bool bval = value.toBool();
        HDEBUG(aKey << "=" << bval);
        (aSettings->*aSetter)(bval);
    }
}

void Database::Private::migrateInt(QSqlDatabase aDb, QString aKey,
    Settings* aSettings, SetInt aSetter)
{
    QVariant value = settingsValue(aDb, aKey);
    if (value.isValid()) {
        bool ok;
        int ival = value.toInt(&ok);
        if (ok) {
            HDEBUG(aKey << "=" << ival);
            (aSettings->*aSetter)(ival);
        } else {
            // JavaScript was storing some integers as floating point
            // e.g. "result_view_duration" = "4.0"
            double dval = value.toDouble(&ok);
            if (ok) {
                ival = round(dval);
                HDEBUG(aKey << "=" << ival);
                (aSettings->*aSetter)(ival);
            } else {
                HWARN("Can't convert" << value.toString() << "to int");
            }
        }
    }
}

void Database::Private::migrateString(QSqlDatabase aDb, QString aKey,
    Settings* aSettings, SetString aSetter)
{
    QVariant value = settingsValue(aDb, aKey);
    if (value.isValid()) {
        QString str(value.toString());
        HDEBUG(aKey << "=" << str);
        (aSettings->*aSetter)(str);
    }
}

// ==========================================================================
// Database
// ==========================================================================

void Database::initialize(QQmlEngine* aEngine, Settings* aSettings)
{
    QDir dir(aEngine->offlineStoragePath() + QDir::separator() +
        QLatin1String("Databases"));
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // Directory for storing the images (don't create it just yet)
    Private::gImageDir = QDir(dir.path() + QDir::separator() +
        QLatin1String("images"));

    // This is how LocalStorage plugin generates database file name
    QCryptographicHash md5(QCryptographicHash::Md5);
    md5.addData(Private::DB_NAME.toUtf8());
    Private::gDatabasePath = dir.path() + QDir::separator() +
        QLatin1String(md5.result().toHex()) + QLatin1String(".sqlite");

    HDEBUG("Database path:" << qPrintable(Private::gDatabasePath));

    QSqlDatabase db = QSqlDatabase::database(Private::DB_NAME);
    if (!db.isValid()) {
        HDEBUG("Adding database" << Private::DB_NAME);
        db = QSqlDatabase::addDatabase(Private::DB_TYPE, Private::DB_NAME);
    }
    db.setDatabaseName(Private::gDatabasePath);

    QStringList tables;
    if (db.open()) {
        tables = db.tables();
        HDEBUG(tables);
    } else {
        HWARN(db.lastError());
    }

    // Check if we need to upgrade or initialize the database
    QString history(HISTORY_TABLE);
    if (tables.contains(history)) {
        // The history table is there, check if we need to upgrade it
        QSqlRecord record(db.record(history));
        if (record.indexOf(HISTORY_FIELD_FORMAT) < 0) {
            // There's no format field, need to add one
            HDEBUG("Adding " HISTORY_FIELD_FORMAT " to the database");
            QSqlQuery query(db);
            query.prepare("ALTER TABLE " HISTORY_TABLE " ADD COLUMN "
                HISTORY_FIELD_FORMAT " TEXT DEFAULT ''");
            if (!query.exec()) {
                HWARN(query.lastError());
            }
            record = db.record(history);
        }
        if (record.indexOf(HISTORY_FIELD_ID) < 0) {
            // QSQLITE can't add a primary key to the existing table.
            // We need to create a new table, copy the old stuff over
            // and rename the table.
            static const char* stmts[] = {
                "CREATE TABLE " HISTORY_TMP_TABLE " ("
                   HISTORY_FIELD_ID " INTEGER PRIMARY KEY AUTOINCREMENT, "
                   HISTORY_FIELD_VALUE " TEXT, "
                   HISTORY_FIELD_TIMESTAMP " TEXT, "
                   HISTORY_FIELD_FORMAT" TEXT)",
               "INSERT INTO " HISTORY_TMP_TABLE "("
                   HISTORY_FIELD_VALUE ", "
                   HISTORY_FIELD_TIMESTAMP ", "
                   HISTORY_FIELD_FORMAT ") SELECT "
                   HISTORY_FIELD_VALUE ", "
                   HISTORY_FIELD_TIMESTAMP ", "
                   HISTORY_FIELD_FORMAT " FROM " HISTORY_TABLE,
               "DROP TABLE " HISTORY_TABLE,
               "ALTER TABLE " HISTORY_TMP_TABLE " RENAME TO " HISTORY_TABLE,
                NULL
            };
            HDEBUG("Adding " HISTORY_FIELD_ID " to the database");
            HVERIFY(db.transaction());
            bool ok = true;
            for (int i = 0; ok && stmts[i]; i++) {
                ok = QSqlQuery(db).exec(QLatin1String(stmts[i]));
            }
            if (ok) {
                HVERIFY(db.commit());
            } else {
                HWARN(db.lastError());
                HVERIFY(db.rollback());
            }
        }
        if (tables.contains(SETTINGS_TABLE)) {
            // The settings table is there, copy those to dconf
            HDEBUG("Migrating settings");
            Private::migrateBool(db, KEY_SOUND,
                aSettings, &Settings::setSound);
            Private::migrateInt(db, KEY_DIGITAL_ZOOM,
                aSettings, &Settings::setDigitalZoom);
            Private::migrateInt(db, KEY_SCAN_DURATION,
                aSettings, &Settings::setScanDuration);
            Private::migrateInt(db, KEY_RESULT_VIEW_DURATION,
                aSettings, &Settings::setResultViewDuration);
            Private::migrateString(db, KEY_MARKER_COLOR,
                aSettings, &Settings::setMarkerColor);
            Private::migrateInt(db, KEY_HISTORY_SIZE,
                aSettings, &Settings::setHistorySize);
            Private::migrateBool(db, KEY_SCAN_ON_START,
                aSettings, &Settings::setScanOnStart);

            // And drop the table when we are done
            QSqlQuery query(db);
            query.prepare("DROP TABLE IF EXISTS " SETTINGS_TABLE);
            if (!query.exec()) {
                HWARN(query.lastError());
            }
        }
    } else {
        // The database doesn't seem to exist at all (fresh install)
        HDEBUG("Initializing the database");
        QSqlQuery query(db);
        if (!query.exec("CREATE TABLE " HISTORY_TABLE " ("
            HISTORY_FIELD_ID " INTEGER PRIMARY KEY AUTOINCREMENT, "
            HISTORY_FIELD_VALUE " TEXT, "
            HISTORY_FIELD_TIMESTAMP " TEXT, "
            HISTORY_FIELD_FORMAT " TEXT)")) {
            HWARN(query.lastError());
        }
    }
}

QSqlDatabase Database::database()
{
    QSqlDatabase db = QSqlDatabase::database(Private::DB_NAME);
    HASSERT(db.isValid());
    return db;
}

QDir Database::imageDir()
{
    return Private::gImageDir;
}
