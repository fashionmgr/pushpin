/*
 * Copyright (C) 2015-2017 Fanout, Inc.
 * Copyright (C) 2024-2025 Fastly, Inc.
 *
 * This file is part of Pushpin.
 *
 * $FANOUT_BEGIN_LICENSE:APACHE2$
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * $FANOUT_END_LICENSE$
 */

#include "updater.h"

#include <QSysInfo>
#include <QDateTime>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QHostInfo>
#include "qtcompat.h"
#include "log.h"
#include "timer.h"
#include "httpheaders.h"
#include "zhttpmanager.h"
#include "zhttprequest.h"

#define CHECK_INTERVAL (24 * 60 * 60 * 1000)
#define REPORT_INTERVAL (15 * 60 * 1000)
#define CHECK_URL "https://updates.fanout.io/check/"
#define USER_AGENT "Pushpin-Updater"
#define MAX_RESPONSE_SIZE 50000

static QString getOs()
{
#if defined(Q_OS_MAC)
	return "mac";
#elif defined(Q_OS_LINUX)
	return "linux";
#elif defined(Q_OS_FREEBSD)
	return "freebsd";
#elif defined(Q_OS_NETBSD)
	return "netbsd";
#elif defined(Q_OS_OPENBSD)
	return "openbsd";
#elif defined(Q_OS_UNIX)
	return "unix";
#else
	return QString();
#endif
}

static QString getArch()
{
	return QString::number(sizeof(void *) * 8);
}

class Updater::Private
{
public:
	struct ReqConnections {
		Connection readyReadConnection;
		Connection errorConnection;
	};

	Updater *q;
	Mode mode;
	bool quiet;
	QString currentVersion;
	QString org;
	ZhttpManager *zhttpManager;
	std::unique_ptr<Timer> timer;
	std::unique_ptr<ZhttpRequest> req;
	Report report;
	QDateTime lastLogTime;
	ReqConnections reqConnections;
	Connection timerConnection;

	Private(Updater *_q, Mode _mode, bool _quiet, const QString &_currentVersion, const QString &_org, ZhttpManager *zhttp) :
		q(_q),
		mode(_mode),
		quiet(_quiet),
		currentVersion(_currentVersion),
		org(_org),
		zhttpManager(zhttp)
	{
		timer = std::make_unique<Timer>();
		timerConnection = timer->timeout.connect(boost::bind(&Private::timer_timeout, this));
		timer->setInterval(mode == ReportMode ? REPORT_INTERVAL : CHECK_INTERVAL);
		timer->start();

		report.connectionsMax = -1; // stale
	}

	void cleanupRequest()
	{
		reqConnections = ReqConnections();
		req.reset();
	}

private:
	void doRequest()
	{
		req = std::unique_ptr<ZhttpRequest>(zhttpManager->createRequest());
		reqConnections = {
			req->readyRead.connect(boost::bind(&Private::req_readyRead, this)),
			req->error.connect(boost::bind(&Private::req_error, this))
		};

		req->setIgnorePolicies(true);
		req->setIgnoreTlsErrors(true);
		req->setQuiet(quiet);

		QUrl url(CHECK_URL);

		QUrlQuery query;
		query.addQueryItem("package", "pushpin");
		query.addQueryItem("version", currentVersion);
		QString os = getOs();
		if(!os.isEmpty())
			query.addQueryItem("os", os);
		QString arch = getArch();
		if(!arch.isEmpty())
			query.addQueryItem("arch", arch);
		if(!org.isEmpty())
			query.addQueryItem("org", org);

		if(mode == ReportMode)
		{
			QString host = QHostInfo::localHostName();
			QString hashedHost = QString::fromUtf8(QCryptographicHash::hash(host.toUtf8(), QCryptographicHash::Sha1).toHex());
			query.addQueryItem("id", hashedHost);

			int cmax = (report.connectionsMax > 0 ? report.connectionsMax : 0);
			query.addQueryItem("cmax", QString::number(cmax));
			query.addQueryItem("cminutes", QString::number(report.connectionsMinutes));
			query.addQueryItem("recv", QString::number(report.messagesReceived));
			query.addQueryItem("sent", QString::number(report.messagesSent));
			query.addQueryItem("ops", QString::number(report.ops));

			report.connectionsMax = -1; // stale
			report.connectionsMinutes = 0;
			report.messagesReceived = 0;
			report.messagesSent = 0;
			report.ops = 0;
		}

		url.setQuery(query);

		HttpHeaders headers;
#ifdef USER_AGENT
		headers += HttpHeader("User-Agent", USER_AGENT);
#endif
		log_debug("updater: checking for updates: %s", qPrintable(url.toString()));
		req->start("GET", url, headers);
		req->endBody();
	}

	void req_readyRead()
	{
		if(req->bytesAvailable() > MAX_RESPONSE_SIZE)
		{
			log_debug("updater: check failed, response too large");
			cleanupRequest();
			return;
		}

		if(!req->isFinished())
			return;

		if(req->responseCode() != 200)
		{
			log_debug("updater: check failed, response code: %d", req->responseCode());
			cleanupRequest();
			return;
		}

		QByteArray rawBody = req->readBody();
		cleanupRequest();

		QJsonParseError e;
		QJsonDocument doc = QJsonDocument::fromJson(rawBody, &e);
		if(e.error != QJsonParseError::NoError || !doc.isObject())
		{
			log_debug("updater: check failed, unexpected response body format");
			return;
		}

		log_debug("updater: check finished");

		QVariantMap body = doc.object().toVariantMap();

		if(body.contains("updates") && typeId(body["updates"]) == QMetaType::QVariantList)
		{
			QVariantList updates = body["updates"].toList();
			if(!updates.isEmpty() && typeId(updates[0]) == QMetaType::QVariantMap)
			{
				QVariantMap update = updates[0].toMap();
				QString version = update.value("version").toString();
				QString link = update.value("link").toString();

				QDateTime now = QDateTime::currentDateTime();

				if(!version.isEmpty() && (lastLogTime.isNull() || now >= lastLogTime.addMSecs(CHECK_INTERVAL - (REPORT_INTERVAL / 2))))
				{
					lastLogTime = now;

					QString msg = QString("New version of Pushpin available! version=%1").arg(version);
					if(!link.isEmpty())
						msg += QString(" %1").arg(link);
					log_info("%s", qPrintable(msg));
				}
			}
		}
	}

	void req_error()
	{
		log_debug("updater: check failed, req error: %d", (int)req->errorCondition());
		cleanupRequest();
	}

	void timer_timeout()
	{
		if(!req)
			doRequest();
	}
};

Updater::Updater(Mode mode, bool quiet, const QString &currentVersion, const QString &org, ZhttpManager *zhttp)
{
	d = new Private(this, mode, quiet, currentVersion, org, zhttp);
}

Updater::~Updater()
{
	delete d;
}

void Updater::setReport(const Report &report)
{
	// update the current report data

	if(d->report.connectionsMax == -1 || report.connectionsMax > d->report.connectionsMax)
		d->report.connectionsMax = report.connectionsMax;

	d->report.connectionsMinutes += report.connectionsMinutes;
	d->report.messagesReceived += report.messagesReceived;
	d->report.messagesSent += report.messagesSent;
	d->report.ops += report.ops;
}
