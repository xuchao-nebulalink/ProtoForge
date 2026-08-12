#include <core/HexUtils.h>
#include <transport/LoopbackTransport.h>
#include <transport/TcpClientTransport.h>
#include <transport/TcpServerTransport.h>
#include <transport/TransportConfig.h>
#include <transport/TransportFactory.h>
#include <transport/TransportThread.h>
#include <transport/UdpTransport.h>

#include <QSignalSpy>
#include <QTest>

using namespace hwsim::core;
using namespace hwsim::transport;

namespace {

/// Collects everything a link reports, so tests can assert on traffic without
/// re-implementing the plumbing each time.
class LinkRecorder : public QObject {
public:
    explicit LinkRecorder(ITransport* transport)
    {
        connect(transport, &ITransport::linkOpened, this, [this](ILink* link) {
            links.append(link);
            connect(link, &ILink::bytesReceived, this,
                    [this](LinkId, const QByteArray& data) { received.append(data); });
        });
        connect(transport, &ITransport::linkClosed, this,
                [this](LinkId, const QString& reason) { closeReasons.append(reason); });
    }

    QList<ILink*> links;
    QList<QByteArray> received;
    QStringList closeReasons;

    [[nodiscard]] QByteArray allReceived() const
    {
        QByteArray joined;
        for (const QByteArray& chunk : received) {
            joined.append(chunk);
        }
        return joined;
    }
};

} // namespace

class TransportTest : public QObject {
    Q_OBJECT

private slots:

    // --- Configuration -----------------------------------------------------

    void configRoundTripsThroughJson()
    {
        TransportConfig config(TransportKind::TcpServer);
        config.setValue(QStringLiteral("port"), 1502);
        config.setValue(QStringLiteral("bindAddress"), QStringLiteral("127.0.0.1"));

        const auto restored = TransportConfig::fromJson(config.toJson());
        QVERIFY(restored.hasValue());
        QCOMPARE(restored.value().kind(), TransportKind::TcpServer);
        QCOMPARE(restored.value().port(), quint16{1502});
    }

    void configRejectsOutOfRangePort()
    {
        TransportConfig config(TransportKind::TcpClient);
        // Bypass normalise() so validate() sees the raw value.
        QVariantMap values = config.values();
        values.insert(QStringLiteral("port"), 999999);
        config.setValues(values);

        QVERIFY(config.validate().hasError());
    }

    void configDescribesItself()
    {
        TransportConfig config(TransportKind::Serial);
        config.setValue(QStringLiteral("portName"), QStringLiteral("COM7"));
        config.setValue(QStringLiteral("baudRate"), QStringLiteral("115200"));
        QVERIFY(config.describe().contains(QStringLiteral("COM7")));
    }

    void everyKindHasASchema()
    {
        for (const TransportKind kind : TransportFactory::instance().availableKinds()) {
            QVERIFY2(!TransportConfig::schemaFor(kind).isEmpty()
                         || kind == TransportKind::Loopback,
                     qPrintable(transportKindName(kind)));
        }
    }

    // --- Factory -----------------------------------------------------------

    void factoryCreatesEveryRegisteredKind()
    {
        auto& factory = TransportFactory::instance();
        QVERIFY(factory.availableKinds().size() >= 5);

        for (const TransportKind kind : factory.availableKinds()) {
            const auto created = factory.create(kind);
            QVERIFY2(created.hasValue(), qPrintable(transportKindName(kind)));
            QCOMPARE(created.value()->kind(), kind);
        }
    }

    void factoryReportsUnknownKind()
    {
        const auto created = TransportFactory::instance().create(QStringLiteral("carrier-pigeon"));
        QVERIFY(created.hasError());
        QCOMPARE(created.error().code, ErrorCode::NotFound);
    }

    // --- Loopback ----------------------------------------------------------

    void loopbackDeliversBothWays()
    {
        LoopbackTransport alpha;
        LoopbackTransport beta;
        alpha.setName(QStringLiteral("alpha"));
        beta.setName(QStringLiteral("beta"));

        QVERIFY(alpha.open(TransportConfig(TransportKind::Loopback)).hasValue());
        QVERIFY(beta.open(TransportConfig(TransportKind::Loopback)).hasValue());

        LinkRecorder alphaRecorder(&alpha);
        LinkRecorder betaRecorder(&beta);

        LoopbackTransport::connectPair(&alpha, &beta);

        QCOMPARE(alpha.linkCount(), qsizetype{1});
        QCOMPARE(beta.linkCount(), qsizetype{1});

        alpha.primaryLink()->send(QByteArray::fromHex("DEADBEEF"));
        QTRY_COMPARE(betaRecorder.allReceived(), QByteArray::fromHex("DEADBEEF"));

        beta.primaryLink()->send(QByteArray::fromHex("0102"));
        QTRY_COMPARE(alphaRecorder.allReceived(), QByteArray::fromHex("0102"));
    }

    void loopbackDeliveryIsAlwaysDeferred()
    {
        LoopbackTransport alpha;
        LoopbackTransport beta;
        QVERIFY(alpha.open(TransportConfig(TransportKind::Loopback)).hasValue());
        QVERIFY(beta.open(TransportConfig(TransportKind::Loopback)).hasValue());

        LinkRecorder betaRecorder(&beta);
        LoopbackTransport::connectPair(&alpha, &beta);

        alpha.primaryLink()->send(QByteArray("x"));
        // Nothing yet: delivery goes through the event loop so a responder can
        // safely reply from inside its own read handler.
        QVERIFY(betaRecorder.received.isEmpty());

        QTRY_COMPARE(betaRecorder.received.size(), 1);
    }

    void loopbackUpdatesStatistics()
    {
        LoopbackTransport alpha;
        LoopbackTransport beta;
        QVERIFY(alpha.open(TransportConfig(TransportKind::Loopback)).hasValue());
        QVERIFY(beta.open(TransportConfig(TransportKind::Loopback)).hasValue());
        LoopbackTransport::connectPair(&alpha, &beta);

        alpha.primaryLink()->send(QByteArray(10, 'a'));
        QCOMPARE(alpha.primaryLink()->statistics().bytesSent, quint64{10});

        QTRY_COMPARE(beta.primaryLink()->statistics().bytesReceived, quint64{10});
    }

    void loopbackDisconnectClosesBothLinks()
    {
        LoopbackTransport alpha;
        LoopbackTransport beta;
        QVERIFY(alpha.open(TransportConfig(TransportKind::Loopback)).hasValue());
        QVERIFY(beta.open(TransportConfig(TransportKind::Loopback)).hasValue());
        LoopbackTransport::connectPair(&alpha, &beta);

        alpha.disconnectPeer();
        QCOMPARE(alpha.linkCount(), qsizetype{0});
        QCOMPARE(beta.linkCount(), qsizetype{0});
    }

    // --- TCP ---------------------------------------------------------------

    void tcpServerAcceptsClientAndExchangesData()
    {
        TcpServerTransport server;
        TransportConfig serverConfig(TransportKind::TcpServer);
        serverConfig.setValue(QStringLiteral("bindAddress"), QStringLiteral("127.0.0.1"));
        serverConfig.setValue(QStringLiteral("port"), 0);  // let the OS pick
        QVERIFY(server.open(serverConfig).hasValue());

        const quint16 port = server.boundPort();
        QVERIFY(port != 0);

        LinkRecorder serverRecorder(&server);

        TcpClientTransport client;
        TransportConfig clientConfig(TransportKind::TcpClient);
        clientConfig.setValue(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        clientConfig.setValue(QStringLiteral("port"), port);
        clientConfig.setValue(QStringLiteral("autoReconnect"), false);
        QVERIFY(client.open(clientConfig).hasValue());

        LinkRecorder clientRecorder(&client);

        QTRY_COMPARE(server.linkCount(), qsizetype{1});
        QTRY_COMPARE(client.linkCount(), qsizetype{1});

        client.primaryLink()->send(QByteArray::fromHex("01030000000A"));
        QTRY_COMPARE(serverRecorder.allReceived(), QByteArray::fromHex("01030000000A"));

        server.primaryLink()->send(QByteArray::fromHex("010302ABCD"));
        QTRY_COMPARE(clientRecorder.allReceived(), QByteArray::fromHex("010302ABCD"));
    }

    void tcpServerEnforcesConnectionLimit()
    {
        TcpServerTransport server;
        TransportConfig config(TransportKind::TcpServer);
        config.setValue(QStringLiteral("bindAddress"), QStringLiteral("127.0.0.1"));
        config.setValue(QStringLiteral("port"), 0);
        config.setValue(QStringLiteral("maxConnections"), 1);
        QVERIFY(server.open(config).hasValue());

        const quint16 port = server.boundPort();

        TcpClientTransport first;
        TcpClientTransport second;
        for (TcpClientTransport* client : {&first, &second}) {
            TransportConfig clientConfig(TransportKind::TcpClient);
            clientConfig.setValue(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
            clientConfig.setValue(QStringLiteral("port"), port);
            clientConfig.setValue(QStringLiteral("autoReconnect"), false);
            QVERIFY(client->open(clientConfig).hasValue());
        }

        QTRY_COMPARE(server.linkCount(), qsizetype{1});
        // Give the rejected connection a chance to appear if the limit failed.
        QTest::qWait(200);
        QCOMPARE(server.linkCount(), qsizetype{1});
    }

    void tcpClientReportsRefusedConnection()
    {
        TcpClientTransport client;
        TransportConfig config(TransportKind::TcpClient);
        config.setValue(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        config.setValue(QStringLiteral("port"), 1);  // nothing listens here
        config.setValue(QStringLiteral("autoReconnect"), false);

        QSignalSpy errorSpy(&client, &ITransport::errorOccurred);
        QVERIFY(client.open(config).hasValue());

        QTRY_VERIFY(!errorSpy.isEmpty());
        QCOMPARE(client.linkCount(), qsizetype{0});
    }

    // --- UDP ---------------------------------------------------------------

    void udpSynthesisesALinkPerPeer()
    {
        UdpTransport receiver;
        TransportConfig receiverConfig(TransportKind::Udp);
        receiverConfig.setValue(QStringLiteral("bindAddress"), QStringLiteral("127.0.0.1"));
        receiverConfig.setValue(QStringLiteral("localPort"), 0);
        QVERIFY(receiver.open(receiverConfig).hasValue());

        const quint16 port = receiver.boundPort();
        QVERIFY(port != 0);

        LinkRecorder receiverRecorder(&receiver);

        const auto makeSender = [port](UdpTransport& sender) {
            TransportConfig config(TransportKind::Udp);
            config.setValue(QStringLiteral("bindAddress"), QStringLiteral("127.0.0.1"));
            config.setValue(QStringLiteral("localPort"), 0);
            config.setValue(QStringLiteral("peerMode"), QStringLiteral("fixed"));
            config.setValue(QStringLiteral("remoteHost"), QStringLiteral("127.0.0.1"));
            config.setValue(QStringLiteral("remotePort"), port);
            return sender.open(config);
        };

        UdpTransport senderA;
        UdpTransport senderB;
        QVERIFY(makeSender(senderA).hasValue());
        QVERIFY(makeSender(senderB).hasValue());

        senderA.primaryLink()->send(QByteArray("from-a"));
        senderB.primaryLink()->send(QByteArray("from-b"));

        // Two distinct source ports means two synthesised links.
        QTRY_COMPARE(receiver.linkCount(), qsizetype{2});
    }

    void udpFixedPeerRaisesLinkImmediately()
    {
        UdpTransport sender;
        TransportConfig config(TransportKind::Udp);
        config.setValue(QStringLiteral("bindAddress"), QStringLiteral("127.0.0.1"));
        config.setValue(QStringLiteral("localPort"), 0);
        config.setValue(QStringLiteral("peerMode"), QStringLiteral("fixed"));
        config.setValue(QStringLiteral("remoteHost"), QStringLiteral("127.0.0.1"));
        config.setValue(QStringLiteral("remotePort"), 9999);

        QVERIFY(sender.open(config).hasValue());
        QCOMPARE(sender.linkCount(), qsizetype{1});
    }

    void udpFixedPeerNeedsARemoteHost()
    {
        UdpTransport sender;
        TransportConfig config(TransportKind::Udp);
        config.setValue(QStringLiteral("localPort"), 0);
        config.setValue(QStringLiteral("peerMode"), QStringLiteral("fixed"));
        config.setValue(QStringLiteral("remoteHost"), QString());

        QVERIFY(sender.open(config).hasError());
    }

    // --- Threading ---------------------------------------------------------

    void transportThreadRunsWorkOnItsOwnThread()
    {
        TransportThread worker(QStringLiteral("io-test"));
        worker.start();
        QVERIFY(worker.isRunning());

        QThread* observed = nullptr;
        worker.invokeBlocking([&observed] { observed = QThread::currentThread(); });

        QVERIFY(observed != nullptr);
        QCOMPARE(observed, worker.workerThread());
        QVERIFY(observed != QThread::currentThread());

        worker.stop();
        QVERIFY(!worker.isRunning());
    }

    void transportThreadInvokeBlockingIsReentrant()
    {
        TransportThread worker(QStringLiteral("io-reentrant"));
        worker.start();

        bool inner = false;
        worker.invokeBlocking([&worker, &inner] {
            // Calling back into invokeBlocking from the worker must run inline
            // rather than deadlock on itself.
            worker.invokeBlocking([&inner] { inner = true; });
        });

        QVERIFY(inner);
        worker.stop();
    }
};

QTEST_MAIN(TransportTest)
#include "tst_transport.moc"
