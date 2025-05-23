/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 * Copyright (C) 2023-2025 Fastly, Inc.
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

#include "app.h"

#include <assert.h>
#include <thread>
#include <pthread.h>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QStringList>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QWaitCondition>
#include "eventloop.h"
#include "processquit.h"
#include "timer.h"
#include "defercall.h"
#include "log.h"
#include "simplehttpserver.h"
#include "settings.h"
#include "xffrule.h"
#include "domainmap.h"
#include "engine.h"
#include "config.h"

static void trimlist(QStringList *list)
{
	for(int n = 0; n < list->count(); ++n)
	{
		if((*list)[n].isEmpty())
		{
			list->removeAt(n);
			--n; // adjust position
		}
	}
}

static XffRule parse_xffRule(const QStringList &in)
{
	XffRule out;
	foreach(const QString &s, in)
	{
		if(s.startsWith("truncate:"))
		{
			bool ok;
			int x = s.mid(9).toInt(&ok);
			if(!ok)
				return out;

			out.truncate = x;
		}
		else if(s == "append")
			out.append = true;
	}
	return out;
}

static QString suffixSpec(const QString &s, int i)
{
	if(s.startsWith("ipc:"))
		return s + QString("-%1").arg(i);

	return s;
}

static QStringList suffixSpecs(const QStringList &l, int i)
{
	if(l.count() == 1 && l[0].startsWith("ipc:"))
		return QStringList() << (l[0] + QString("-%1").arg(i));

	return l;
}

enum CommandLineParseResult
{
	CommandLineOk,
	CommandLineError,
	CommandLineVersionRequested,
	CommandLineHelpRequested
};

class ArgsData
{
public:
	QString configFile;
	QString logFile;
	int logLevel;
	QString ipcPrefix;
	QStringList routeLines;
	bool quietCheck;

	ArgsData() :
		logLevel(-1),
		quietCheck(false)
	{
	}
};

static CommandLineParseResult parseCommandLine(QCommandLineParser *parser, ArgsData *args, QString *errorMessage)
{
	parser->setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
	const QCommandLineOption configFileOption("config", "Config file.", "file");
	parser->addOption(configFileOption);
	const QCommandLineOption logFileOption("logfile", "File to log to.", "file");
	parser->addOption(logFileOption);
	const QCommandLineOption logLevelOption("loglevel", "Log level (default: 2).", "x");
	parser->addOption(logLevelOption);
	const QCommandLineOption verboseOption("verbose", "Verbose output. Same as --loglevel=3.");
	parser->addOption(verboseOption);
	const QCommandLineOption ipcPrefixOption("ipc-prefix", "Override ipc_prefix config option.", "prefix");
	parser->addOption(ipcPrefixOption);
	const QCommandLineOption routeOption("route", "Add route (overrides routes file).", "line");
	parser->addOption(routeOption);
	const QCommandLineOption quietCheckOption("quiet-check", "Log update checks in Zurl as debug level.");
	parser->addOption(quietCheckOption);
	const QCommandLineOption helpOption = parser->addHelpOption();
	const QCommandLineOption versionOption = parser->addVersionOption();

	if(!parser->parse(QCoreApplication::arguments()))
	{
		*errorMessage = parser->errorText();
		return CommandLineError;
	}

	if(parser->isSet(versionOption))
		return CommandLineVersionRequested;

	if(parser->isSet(helpOption))
		return CommandLineHelpRequested;

	if(parser->isSet(configFileOption))
		args->configFile = parser->value(configFileOption);

	if(parser->isSet(logFileOption))
		args->logFile = parser->value(logFileOption);

	if(parser->isSet(logLevelOption))
	{
		bool ok;
		int x = parser->value(logLevelOption).toInt(&ok);
		if(!ok || x < 0)
		{
			*errorMessage = "error: loglevel must be greater than or equal to 0";
			return CommandLineError;
		}

		args->logLevel = x;
	}

	if(parser->isSet(verboseOption))
		args->logLevel = 3;

	if(parser->isSet(ipcPrefixOption))
		args->ipcPrefix = parser->value(ipcPrefixOption);

	if(parser->isSet(routeOption))
	{
		foreach(const QString &r, parser->values(routeOption))
			args->routeLines += r;
	}

	if(parser->isSet(quietCheckOption))
		args->quietCheck = true;

	return CommandLineOk;
}

class EngineWorker
{
public:
	EngineWorker(const Engine::Configuration &config, DomainMap *domainMap) :
		config_(config),
		engine_(std::make_unique<Engine>(domainMap))
	{
	}

	DeferCall deferCall;

	Signal started;
	Signal stopped;
	Signal error;

	void start()
	{
		if(!engine_->start(config_))
		{
			engine_.reset();

			error();
			return;
		}

		started();
	}

	void stop()
	{
		engine_.reset();

		stopped();
	}

	void routesChanged()
	{
		if(engine_)
			engine_->routesChanged();
	}

private:
	Engine::Configuration config_;
	std::unique_ptr<Engine> engine_;
};

class EngineThread
{
public:
	std::thread thread;
	QMutex m;
	QWaitCondition w;
	Engine::Configuration config;
	DomainMap *domainMap;
	bool newEventLoop;
	std::unique_ptr<EngineWorker> worker;

	EngineThread(const Engine::Configuration &_config, DomainMap *_domainMap, bool _newEventLoop) :
		config(_config),
		domainMap(_domainMap),
		newEventLoop(_newEventLoop)
	{
	}

	~EngineThread()
	{
		stop();
		thread.join();
	}

	bool start()
	{
		QString name = "proxy-worker-" + QString::number(config.id);

		QMutexLocker locker(&m);

		thread = std::thread([=] {
#ifdef Q_OS_MAC
			pthread_setname_np(name.toUtf8().data());
#else
			pthread_setname_np(pthread_self(), name.toUtf8().data());
#endif

			run();
		});

		w.wait(&m);
		return (bool)worker;
	}

	void stop()
	{
		QMutexLocker locker(&m);

		if(worker)
		{
			worker->deferCall.defer([=] {
				// NOTE: called from worker thread
				worker->stop();
			});
		}
	}

	void routesChanged()
	{
		QMutexLocker locker(&m);

		if(worker)
		{
			worker->deferCall.defer([=] {
				// NOTE: called from worker thread
				worker->routesChanged();
			});
		}
	}

	void run()
	{
		// will unlock during exec
		m.lock();

		// enough timers for sessions and zroutes, plus an extra 100 for misc
		int timersMax = (config.sessionsMax * TIMERS_PER_SESSION) + (ZROUTES_MAX * TIMERS_PER_ZROUTE) + 100;

		std::unique_ptr<EventLoop> loop;
		std::unique_ptr<QEventLoop> qloop;

		if(newEventLoop)
		{
			log_debug("worker %d: using new event loop", config.id);

			// enough for zroutes and prometheus requests, plus an extra 100 for misc
			int socketNotifiersMax = (SOCKETNOTIFIERS_PER_ZROUTE * ZROUTES_MAX) + (SOCKETNOTIFIERS_PER_SIMPLEHTTPREQUEST * PROMETHEUS_CONNECTIONS_MAX) + 100;

			int registrationsMax = timersMax + socketNotifiersMax;
			loop = std::make_unique<EventLoop>(registrationsMax);
		}
		else
		{
			// for qt event loop, timer subsystem must be explicitly initialized
			Timer::init(timersMax);

			qloop = std::make_unique<QEventLoop>();
		}

		worker = std::make_unique<EngineWorker>(config, domainMap);

		worker->started.connect([&] {
			log_debug("worker %d: started", config.id);

			// unblock start()
			w.wakeOne();
			m.unlock();
		});

		worker->stopped.connect([&] {
			worker.reset();

			log_debug("worker %d: stopped", config.id);

			if(newEventLoop)
				loop->exit(0);
			else
				qloop->quit();
		});

		worker->error.connect([&] {
			worker.reset();

			if(newEventLoop)
				loop->exit(0);
			else
				qloop->quit();

			// unblock start()
			w.wakeOne();
			m.unlock();
		});

		worker->deferCall.defer([=] { worker->start(); });

		if(newEventLoop)
			loop->exec();
		else
			qloop->exec();

		if(!newEventLoop)
		{
			// ensure deferred deletes are processed
			QCoreApplication::instance()->sendPostedEvents();
		}

		// deinit here, after all event loop activity has completed

		DeferCall::cleanup();

		if(!newEventLoop)
			Timer::deinit();
	}
};

class App::Private
{
public:
	static int run()
	{
		QCoreApplication::setApplicationName("pushpin-proxy");
		QCoreApplication::setApplicationVersion(Config::get().version);

		QCommandLineParser parser;
		parser.setApplicationDescription("Pushpin proxy component.");

		ArgsData args;
		QString errorMessage;
		switch(parseCommandLine(&parser, &args, &errorMessage))
		{
			case CommandLineOk:
				break;
			case CommandLineError:
				fprintf(stderr, "%s\n\n%s", qPrintable(errorMessage), qPrintable(parser.helpText()));
				return 1;
			case CommandLineVersionRequested:
				printf("%s %s\n", qPrintable(QCoreApplication::applicationName()),
					qPrintable(QCoreApplication::applicationVersion()));
				return 0;
			case CommandLineHelpRequested:
				parser.showHelp();
				Q_UNREACHABLE();
		}

		if(args.logLevel != -1)
			log_setOutputLevel(args.logLevel);
		else
			log_setOutputLevel(LOG_LEVEL_INFO);

		if(!args.logFile.isEmpty())
		{
			if(!log_setFile(args.logFile))
			{
				log_error("failed to open log file: %s", qPrintable(args.logFile));
				return 1;
			}
		}

		log_debug("starting...");

		QString configFile = args.configFile;
		if(configFile.isEmpty())
			configFile = QDir(Config::get().configDir).filePath("pushpin.conf");

		// QSettings doesn't inform us if the config file doesn't exist, so do that ourselves
		{
			QFile file(configFile);
			if(!file.open(QIODevice::ReadOnly))
			{
				log_error("failed to open %s, and --config not passed", qPrintable(configFile));
				return 1;
			}
		}

		QDir configDir = QFileInfo(configFile).absoluteDir();

		Settings settings(configFile);

		if(!args.ipcPrefix.isEmpty())
			settings.setIpcPrefix(args.ipcPrefix);

		QStringList services = settings.value("runner/services").toStringList();

		int workerCount = settings.value("proxy/workers", 1).toInt();
		QStringList connmgr_in_specs = settings.value("proxy/connmgr_in_specs").toStringList();
		trimlist(&connmgr_in_specs);
		QStringList connmgr_in_stream_specs = settings.value("proxy/connmgr_in_stream_specs").toStringList();
		trimlist(&connmgr_in_stream_specs);
		QStringList connmgr_out_specs = settings.value("proxy/connmgr_out_specs").toStringList();
		trimlist(&connmgr_out_specs);
		QStringList condure_in_specs = settings.value("proxy/condure_in_specs").toStringList();
		trimlist(&condure_in_specs);
		connmgr_in_specs += condure_in_specs;
		QStringList condure_in_stream_specs = settings.value("proxy/condure_in_stream_specs").toStringList();
		trimlist(&condure_in_stream_specs);
		connmgr_in_stream_specs += condure_in_stream_specs;
		QStringList condure_out_specs = settings.value("proxy/condure_out_specs").toStringList();
		trimlist(&condure_out_specs);
		connmgr_out_specs += condure_out_specs;
		QStringList m2a_in_specs = settings.value("proxy/m2a_in_specs").toStringList();
		trimlist(&m2a_in_specs);
		QStringList m2a_in_stream_specs = settings.value("proxy/m2a_in_stream_specs").toStringList();
		trimlist(&m2a_in_stream_specs);
		QStringList m2a_out_specs = settings.value("proxy/m2a_out_specs").toStringList();
		trimlist(&m2a_out_specs);
		QStringList connmgr_client_out_specs = settings.value("proxy/connmgr_client_out_specs").toStringList();
		trimlist(&connmgr_client_out_specs);
		QStringList connmgr_client_out_stream_specs = settings.value("proxy/connmgr_client_out_stream_specs").toStringList();
		trimlist(&connmgr_client_out_stream_specs);
		QStringList connmgr_client_in_specs = settings.value("proxy/connmgr_client_in_specs").toStringList();
		trimlist(&connmgr_client_in_specs);
		QStringList condure_client_out_specs = settings.value("proxy/condure_client_out_specs").toStringList();
		trimlist(&condure_client_out_specs);
		connmgr_client_out_specs += condure_client_out_specs;
		QStringList condure_client_out_stream_specs = settings.value("proxy/condure_client_out_stream_specs").toStringList();
		trimlist(&condure_client_out_stream_specs);
		connmgr_client_out_stream_specs += condure_client_out_stream_specs;
		QStringList condure_client_in_specs = settings.value("proxy/condure_client_in_specs").toStringList();
		trimlist(&condure_client_in_specs);
		connmgr_client_in_specs += condure_client_in_specs;
		QStringList zurl_out_specs = settings.value("proxy/zurl_out_specs").toStringList();
		trimlist(&zurl_out_specs);
		QStringList zurl_out_stream_specs = settings.value("proxy/zurl_out_stream_specs").toStringList();
		trimlist(&zurl_out_stream_specs);
		QStringList zurl_in_specs = settings.value("proxy/zurl_in_specs").toStringList();
		trimlist(&zurl_in_specs);
		QString handler_inspect_spec = settings.value("proxy/handler_inspect_spec").toString();
		QString handler_accept_spec = settings.value("proxy/handler_accept_spec").toString();
		QString handler_retry_in_spec = settings.value("proxy/handler_retry_in_spec").toString();
		QStringList handler_ws_control_init_specs = settings.value("proxy/handler_ws_control_init_specs").toStringList();
		trimlist(&handler_ws_control_init_specs);
		QStringList handler_ws_control_stream_specs = settings.value("proxy/handler_ws_control_stream_specs").toStringList();
		trimlist(&handler_ws_control_stream_specs);
		QString stats_spec = settings.value("proxy/stats_spec").toString();
		QString command_spec = settings.value("proxy/command_spec").toString();
		QStringList intreq_in_specs = settings.value("proxy/intreq_in_specs").toStringList();
		trimlist(&intreq_in_specs);
		QStringList intreq_in_stream_specs = settings.value("proxy/intreq_in_stream_specs").toStringList();
		trimlist(&intreq_in_stream_specs);
		QStringList intreq_out_specs = settings.value("proxy/intreq_out_specs").toStringList();
		trimlist(&intreq_out_specs);
		bool ok;
		int ipcFileMode = settings.value("proxy/ipc_file_mode", -1).toString().toInt(&ok, 8);
		int sessionsMax = settings.value("proxy/max_open_requests", -1).toInt();
		QString routesFile = settings.value("proxy/routesfile").toString();
		bool debug = settings.value("proxy/debug").toBool();
		bool autoCrossOrigin = settings.value("proxy/auto_cross_origin").toBool();
		bool acceptXForwardedProtocol = settings.value("proxy/accept_x_forwarded_protocol").toBool();
		QString setXForwardedProtocol = settings.value("proxy/set_x_forwarded_protocol").toString();
		bool setXfProto = (setXForwardedProtocol == "true" || setXForwardedProtocol == "proto-only");
		bool setXfProtocol = (setXForwardedProtocol == "true");
		XffRule xffRule = parse_xffRule(settings.value("proxy/x_forwarded_for").toStringList());
		XffRule xffTrustedRule = parse_xffRule(settings.value("proxy/x_forwarded_for_trusted").toStringList());
		QStringList origHeadersNeedMarkStr = settings.value("proxy/orig_headers_need_mark").toStringList();
		trimlist(&origHeadersNeedMarkStr);
		bool acceptPushpinRoute = settings.value("proxy/accept_pushpin_route").toBool();
		QByteArray cdnLoop = settings.value("proxy/cdn_loop").toString().toUtf8();
		bool logFrom = settings.value("proxy/log_from").toBool();
		bool logUserAgent = settings.value("proxy/log_user_agent").toBool();
		QByteArray sigIss = settings.value("proxy/sig_iss", "pushpin").toString().toUtf8();
		Jwt::EncodingKey sigKey = Jwt::EncodingKey::fromConfigString(settings.value("proxy/sig_key").toString(), configDir);
		Jwt::DecodingKey upstreamKey = Jwt::DecodingKey::fromConfigString(settings.value("proxy/upstream_key").toString(), configDir);
		QString sockJsUrl = settings.value("proxy/sockjs_url").toString();
		QString updatesCheck = settings.value("proxy/updates_check").toString();
		QString organizationName = settings.value("proxy/organization_name").toString();
		int clientMaxconn = settings.value("runner/client_maxconn", 50000).toInt();
		bool statsConnectionSend = settings.value("global/stats_connection_send", true).toBool();
		int statsConnectionTtl = settings.value("global/stats_connection_ttl", 120).toInt();
		int statsConnectionsMaxTtl = settings.value("proxy/stats_connections_max_ttl", 60).toInt();
		int statsReportInterval = settings.value("proxy/stats_report_interval", 10).toInt();
		QString prometheusPort = settings.value("proxy/prometheus_port").toString();
		QString prometheusPrefix = settings.value("proxy/prometheus_prefix").toString();
		bool newEventLoop = settings.value("proxy/new_event_loop", false).toBool();

		QList<QByteArray> origHeadersNeedMark;
		foreach(const QString &s, origHeadersNeedMarkStr)
			origHeadersNeedMark += s.toUtf8();

		// if routesfile is a relative path, then use it relative to the config file location
		QFileInfo fi(routesFile);
		if(fi.isRelative())
			routesFile = QFileInfo(configDir, routesFile).filePath();

		if(!(!connmgr_in_specs.isEmpty() && !connmgr_in_stream_specs.isEmpty() && !connmgr_out_specs.isEmpty()) && !(!m2a_in_specs.isEmpty() && !m2a_in_stream_specs.isEmpty() && !m2a_out_specs.isEmpty()))
		{
			log_error("must set connmgr_in_specs, connmgr_in_stream_specs, and connmgr_out_specs, or m2a_in_specs, m2a_in_stream_specs, and m2a_out_specs");
			return 1;
		}

		if(!(!connmgr_client_out_specs.isEmpty() && !connmgr_client_out_stream_specs.isEmpty() && !connmgr_client_in_specs.isEmpty()) && !(!zurl_out_specs.isEmpty() && !zurl_out_stream_specs.isEmpty() && !zurl_in_specs.isEmpty()))
		{
			log_error("must set connmgr_client_out_specs, connmgr_client_out_stream_specs, and connmgr_client_in_specs, or zurl_out_specs, zurl_out_stream_specs, and zurl_in_specs");
			return 1;
		}

		if(updatesCheck == "true")
			updatesCheck = "check";

		// sessionsMax should not exceed clientMaxconn
		if(sessionsMax >= 0)
			sessionsMax = qMin(sessionsMax, clientMaxconn);
		else
			sessionsMax = clientMaxconn;

		Engine::Configuration config;
		config.appVersion = Config::get().version;
		config.clientId = "proxy_" + QByteArray::number(QCoreApplication::applicationPid());
		if(!services.contains("mongrel2") && (!connmgr_in_specs.isEmpty() || !connmgr_in_stream_specs.isEmpty() || !connmgr_out_specs.isEmpty()))
		{
			config.serverInSpecs = connmgr_in_specs;
			config.serverInStreamSpecs = connmgr_in_stream_specs;
			config.serverOutSpecs = connmgr_out_specs;
		}
		else
		{
			config.serverInSpecs = m2a_in_specs;
			config.serverInStreamSpecs = m2a_in_stream_specs;
			config.serverOutSpecs = m2a_out_specs;
		}
		if(!services.contains("zurl") && (!connmgr_client_out_specs.isEmpty() || !connmgr_client_out_stream_specs.isEmpty() || !connmgr_client_in_specs.isEmpty()))
		{
			config.clientOutSpecs = connmgr_client_out_specs;
			config.clientOutStreamSpecs = connmgr_client_out_stream_specs;
			config.clientInSpecs = connmgr_client_in_specs;
		}
		else
		{
			config.clientOutSpecs = zurl_out_specs;
			config.clientOutStreamSpecs = zurl_out_stream_specs;
			config.clientInSpecs = zurl_in_specs;
		}
		config.inspectSpec = handler_inspect_spec;
		config.acceptSpec = handler_accept_spec;
		config.retryInSpec = handler_retry_in_spec;
		config.wsControlInitSpecs = handler_ws_control_init_specs;
		config.wsControlStreamSpecs = handler_ws_control_stream_specs;
		config.statsSpec = stats_spec;
		config.commandSpec = command_spec;
		config.intServerInSpecs = intreq_in_specs;
		config.intServerInStreamSpecs = intreq_in_stream_specs;
		config.intServerOutSpecs = intreq_out_specs;
		config.ipcFileMode = ipcFileMode;
		config.sessionsMax = sessionsMax / workerCount;
		config.debug = debug;
		config.autoCrossOrigin = autoCrossOrigin;
		config.acceptXForwardedProto = acceptXForwardedProtocol;
		config.setXForwardedProto = setXfProto;
		config.setXForwardedProtocol = setXfProtocol;
		config.xffUntrustedRule = xffRule;
		config.xffTrustedRule = xffTrustedRule;
		config.origHeadersNeedMark = origHeadersNeedMark;
		config.acceptPushpinRoute = acceptPushpinRoute;
		config.cdnLoop = cdnLoop;
		config.logFrom = logFrom;
		config.logUserAgent = logUserAgent;
		config.sigIss = sigIss;
		config.sigKey = sigKey;
		config.upstreamKey = upstreamKey;
		config.sockJsUrl = sockJsUrl;
		config.updatesCheck = updatesCheck;
		config.organizationName = organizationName;
		config.quietCheck = args.quietCheck;
		config.statsConnectionSend = statsConnectionSend;
		config.statsConnectionTtl = statsConnectionTtl;
		config.statsConnectionsMaxTtl = statsConnectionsMaxTtl;
		config.statsReportInterval = statsReportInterval;
		config.prometheusPort = prometheusPort;
		config.prometheusPrefix = prometheusPrefix;

		return runLoop(config, args.routeLines, routesFile, workerCount, newEventLoop);
	}

private:
	static int runLoop(const Engine::Configuration &config, const QStringList &routeLines, const QString &routesFile, int workerCount, bool newEventLoop)
	{
		// plenty for the main thread
		int timersMax = 100;

		std::unique_ptr<EventLoop> loop;

		if(newEventLoop)
		{
			log_debug("using new event loop");

			// for processquit
			int socketNotifiersMax = 1;

			int registrationsMax = timersMax + socketNotifiersMax;
			loop = std::make_unique<EventLoop>(registrationsMax);
		}
		else
		{
			// for qt event loop, timer subsystem must be explicitly initialized
			Timer::init(timersMax);
		}

		std::unique_ptr<DomainMap> domainMap;
		std::list<EngineThread*> threads;

		DeferCall deferCall;
		deferCall.defer([&] {
			if(!routeLines.isEmpty())
			{
				domainMap = std::make_unique<DomainMap>(newEventLoop);
				foreach(const QString &line, routeLines)
					domainMap->addRouteLine(line);
			}
			else
				domainMap = std::make_unique<DomainMap>(routesFile, newEventLoop);

			domainMap->changed.connect([&] {
				for(EngineThread *t : threads)
					t->routesChanged();
			});

			ProcessQuit::instance()->quit.connect([&] {
				log_info("stopping...");

				// remove the handler, so if we get another signal then we crash out
				ProcessQuit::cleanup();

				for(EngineThread *t : threads)
					t->stop();

				for(EngineThread *t : threads)
					delete t;

				threads.clear();

				log_debug("stopped");

				if(newEventLoop)
					loop->exit(0);
				else
					QCoreApplication::exit(0);
			});

			ProcessQuit::instance()->hup.connect([&] {
				log_info("reloading");
				log_rotate();
				domainMap->reload();
			});

			for(int n = 0; n < workerCount; ++n)
			{
				Engine::Configuration wconfig = config;

				wconfig.id = n;

				if(workerCount > 1)
				{
					wconfig.clientId += '-' + QByteArray::number(n);

					wconfig.inspectSpec = suffixSpec(wconfig.inspectSpec, n);
					wconfig.acceptSpec = suffixSpec(wconfig.acceptSpec, n);
					wconfig.retryInSpec = suffixSpec(wconfig.retryInSpec, n);
					wconfig.wsControlInitSpecs = suffixSpecs(wconfig.wsControlInitSpecs, n);
					wconfig.wsControlStreamSpecs = suffixSpecs(wconfig.wsControlStreamSpecs, n);
					wconfig.statsSpec = suffixSpec(wconfig.statsSpec, n);
					wconfig.commandSpec = suffixSpec(wconfig.commandSpec, n);
					wconfig.intServerInSpecs = suffixSpecs(wconfig.intServerInSpecs, n);
					wconfig.intServerInStreamSpecs = suffixSpecs(wconfig.intServerInStreamSpecs, n);
					wconfig.intServerOutSpecs = suffixSpecs(wconfig.intServerOutSpecs, n);
				}

				EngineThread *t = new EngineThread(wconfig, domainMap.get(), newEventLoop);
				if(!t->start())
				{
					delete t;

					for(EngineThread *t : threads)
						delete t;

					threads.clear();

					if(newEventLoop)
						loop->exit(1);
					else
						QCoreApplication::exit(1);

					return;
				}

				threads.push_back(t);
			}

			log_info("started");
		});

		int ret;
		if(newEventLoop)
			ret = loop->exec();
		else
			ret = QCoreApplication::exec();

		if(!newEventLoop)
		{
			// ensure deferred deletes are processed
			QCoreApplication::instance()->sendPostedEvents();
		}

		// deinit here, after all event loop activity has completed

		DeferCall::cleanup();

		if(!newEventLoop)
			Timer::deinit();

		return ret;
	}
};

App::App() = default;

App::~App() = default;

int App::run()
{
	return Private::run();
}
