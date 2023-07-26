/**
 * SPDX-FileCopyrightText: 2013 Albert Vaca <albertvaka@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "lanlinkprovider.h"
#include "core_debug.h"

#ifndef Q_OS_WIN
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#else
#include <winsock2.h>
// Winsock2 needs to be included before any other header
#include <mstcpip.h>
#endif

#include <QHostInfo>
#include <QMetaEnum>
#include <QNetworkProxy>
#include <QSslCipher>
#include <QSslConfiguration>
#include <QSslKey>
#include <QStringList>
#include <QTcpServer>
#include <QUdpSocket>

#include "daemon.h"
#include "kdeconnectconfig.h"
#include "landevicelink.h"

static const int MAX_UNPAIRED_CONNECTIONS = 42;
static const int MAX_REMEMBERED_IDENTITY_PACKETS = 42;

LanLinkProvider::LanLinkProvider(bool testMode, quint16 udpBroadcastPort, quint16 udpListenPort)
    : m_server(new Server(this))
    , m_udpSocket(this)
    , m_tcpPort(0)
    , m_udpBroadcastPort(udpBroadcastPort)
    , m_udpListenPort(udpListenPort)
    , m_testMode(testMode)
    , m_combineBroadcastsTimer(this)
#ifdef KDECONNECT_MDNS
    , m_mdnsDiscovery(this)
#endif
{
    m_combineBroadcastsTimer.setInterval(0); // increase this if waiting a single event-loop iteration is not enough
    m_combineBroadcastsTimer.setSingleShot(true);
    connect(&m_combineBroadcastsTimer, &QTimer::timeout, this, &LanLinkProvider::broadcastToNetwork);

    connect(&m_udpSocket, &QIODevice::readyRead, this, &LanLinkProvider::udpBroadcastReceived);

    m_server->setProxy(QNetworkProxy::NoProxy);
    connect(m_server, &QTcpServer::newConnection, this, &LanLinkProvider::newConnection);

    m_udpSocket.setProxy(QNetworkProxy::NoProxy);

    connect(&m_udpSocket, &QAbstractSocket::errorOccurred, [](QAbstractSocket::SocketError socketError) {
        qWarning() << "Error sending UDP packet:" << socketError;
    });

#if QT_VERSION_MAJOR < 6
    QNetworkConfigurationManager *networkManager = new QNetworkConfigurationManager(this);
    connect(networkManager, &QNetworkConfigurationManager::configurationChanged, this, [this](QNetworkConfiguration config) {
        if (m_lastConfig != config && config.state() == QNetworkConfiguration::Active) {
            m_lastConfig = config;
            onNetworkChange();
        }
    });
#else
    const auto checkNetworkChange = [this]() {
        if (QNetworkInformation::instance()->reachability() == QNetworkInformation::Reachability::Online) {
            onNetworkChange();
        }
    };
    // Detect when a network interface changes status, so we announce ourselves in the new network
    QNetworkInformation::instance()->loadBackendByFeatures(QNetworkInformation::Feature::Reachability);

    // We want to know if our current network reachability has changed, or if we change from one network to another
    connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this, checkNetworkChange);
    connect(QNetworkInformation::instance(), &QNetworkInformation::transportMediumChanged, this, checkNetworkChange);
#endif
}

LanLinkProvider::~LanLinkProvider()
{
}

void LanLinkProvider::onStart()
{
    const QHostAddress bindAddress = m_testMode ? QHostAddress::LocalHost : QHostAddress::Any;

    bool success = m_udpSocket.bind(bindAddress, m_udpListenPort, QUdpSocket::ShareAddress);
    if (!success) {
        QAbstractSocket::SocketError sockErr = m_udpSocket.error();
        // Refer to https://doc.qt.io/qt-5/qabstractsocket.html#SocketError-enum to decode socket error number
        QString errorMessage = QString::fromLatin1(QMetaEnum::fromType<QAbstractSocket::SocketError>().valueToKey(sockErr));
        qCritical(KDECONNECT_CORE) << QLatin1String("Failed to bind UDP socket on port") << m_udpListenPort << QLatin1String("with error") << errorMessage;
    }
    Q_ASSERT(success);

    m_tcpPort = MIN_TCP_PORT;
    while (!m_server->listen(bindAddress, m_tcpPort)) {
        m_tcpPort++;
        if (m_tcpPort > MAX_TCP_PORT) { // No ports available?
            qCritical(KDECONNECT_CORE) << "Error opening a port in range" << MIN_TCP_PORT << "-" << MAX_TCP_PORT;
            m_tcpPort = 0;
            return;
        }
    }

    broadcastUdpIdentityPacket();

#ifdef KDECONNECT_MDNS
    m_mdnsDiscovery.startAnnouncing();
    m_mdnsDiscovery.startDiscovering();
#endif

    qCDebug(KDECONNECT_CORE) << "LanLinkProvider started";
}

void LanLinkProvider::onStop()
{
#ifdef KDECONNECT_MDNS
    m_mdnsDiscovery.stopAnnouncing();
    m_mdnsDiscovery.stopDiscovering();
#endif
    m_udpSocket.close();
    m_server->close();
    qCDebug(KDECONNECT_CORE) << "LanLinkProvider stopped";
}

void LanLinkProvider::onNetworkChange()
{
    if (m_combineBroadcastsTimer.isActive()) {
        qCDebug(KDECONNECT_CORE) << "Preventing duplicate broadcasts";
        return;
    }
    m_combineBroadcastsTimer.start();
}

// I'm in a new network, let's be polite and introduce myself
void LanLinkProvider::broadcastToNetwork()
{
    if (!m_server->isListening()) {
        qWarning() << "TCP server not listening, not broadcasting";
        return;
    }

    Q_ASSERT(m_tcpPort != 0);

    broadcastUdpIdentityPacket();
#ifdef KDECONNECT_MDNS
    m_mdnsDiscovery.stopDiscovering();
    m_mdnsDiscovery.startDiscovering();
#endif
}

void LanLinkProvider::broadcastUdpIdentityPacket()
{
    if (qEnvironmentVariableIsSet("KDECONNECT_DISABLE_UDP_BROADCAST")) {
        qWarning() << "Not broadcasting UDP because KDECONNECT_DISABLE_UDP_BROADCAST is set";
        return;
    }
    qCDebug(KDECONNECT_CORE()) << "Broadcasting identity packet";

    QList<QHostAddress> addresses = getBroadcastAddresses();

#if defined(Q_OS_WIN) || defined(Q_OS_FREEBSD)
    // On Windows and FreeBSD we need to broadcast from every local IP address to reach all networks
    QUdpSocket sendSocket;
    sendSocket.setProxy(QNetworkProxy::NoProxy);
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if ((iface.flags() & QNetworkInterface::IsUp) && (iface.flags() & QNetworkInterface::IsRunning) && (iface.flags() & QNetworkInterface::CanBroadcast)) {
            for (const QNetworkAddressEntry &ifaceAddress : iface.addressEntries()) {
                QHostAddress sourceAddress = ifaceAddress.ip();
                if (sourceAddress.protocol() == QAbstractSocket::IPv4Protocol && sourceAddress != QHostAddress::LocalHost) {
                    qCDebug(KDECONNECT_CORE()) << "Broadcasting as" << sourceAddress;
                    sendSocket.bind(sourceAddress);
                    sendUdpIdentityPacket(sendSocket, addresses);
                    sendSocket.close();
                }
            }
        }
    }
#else
    sendUdpIdentityPacket(addresses);
#endif
}

QList<QHostAddress> LanLinkProvider::getBroadcastAddresses()
{
    const QStringList customDevices = KdeConnectConfig::instance().customDevices();

    QList<QHostAddress> destinations;
    destinations.reserve(customDevices.length() + 1);

    // Default broadcast address
    destinations.append(m_testMode ? QHostAddress::LocalHost : QHostAddress::Broadcast);

    // Custom device addresses
    for (auto &customDevice : customDevices) {
        QHostAddress address(customDevice);
        if (address.isNull()) {
            qCWarning(KDECONNECT_CORE) << "Invalid custom device address" << customDevice;
        } else {
            destinations.append(address);
        }
    }

    return destinations;
}

void LanLinkProvider::sendUdpIdentityPacket(const QList<QHostAddress> &addresses)
{
    sendUdpIdentityPacket(m_udpSocket, addresses);
}

void LanLinkProvider::sendUdpIdentityPacket(QUdpSocket &socket, const QList<QHostAddress> &addresses)
{
    DeviceInfo myDeviceInfo = KdeConnectConfig::instance().deviceInfo();
    NetworkPacket identityPacket = myDeviceInfo.toIdentityPacket();
    identityPacket.set(QStringLiteral("tcpPort"), m_tcpPort);
    const QByteArray payload = identityPacket.serialize();

    for (auto &address : addresses) {
        qint64 bytes = socket.writeDatagram(payload, address, m_udpBroadcastPort);
        if (bytes == -1 && socket.error() == QAbstractSocket::DatagramTooLargeError) {
            // On macOS and FreeBSD, UDP broadcasts larger than MTU get dropped. See:
            // https://opensource.apple.com/source/xnu/xnu-3789.1.32/bsd/netinet/ip_output.c.auto.html#:~:text=/*%20don%27t%20allow%20broadcast%20messages%20to%20be%20fragmented%20*/
            // We remove the capabilities to reduce the size of the packet.
            // This should only happen for broadcasts, so UDP packets sent from MDNS discoveries should still work.
            qWarning() << "Identity packet to" << address << "got rejected because it was too large. Retrying without including the capabilities";
            identityPacket.set(QStringLiteral("outgoingCapabilities"), QStringList());
            identityPacket.set(QStringLiteral("incomingCapabilities"), QStringList());
            const QByteArray smallPayload = identityPacket.serialize();
            socket.writeDatagram(smallPayload, address, m_udpBroadcastPort);
        }
    }
}

// I'm the existing device, a new device is kindly introducing itself.
// I will create a TcpSocket and try to connect. This can result in either tcpSocketConnected() or connectError().
void LanLinkProvider::udpBroadcastReceived()
{
    while (m_udpSocket.hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpSocket.pendingDatagramSize());
        QHostAddress sender;

        m_udpSocket.readDatagram(datagram.data(), datagram.size(), &sender);

        if (sender.isLoopback() && !m_testMode)
            continue;

        NetworkPacket *receivedPacket = new NetworkPacket();
        bool success = NetworkPacket::unserialize(datagram, receivedPacket);

        // qCDebug(KDECONNECT_CORE) << "Datagram " << datagram.data() ;

        if (!success) {
            qCDebug(KDECONNECT_CORE) << "Could not unserialize UDP packet";
            delete receivedPacket;
            continue;
        }

        if (receivedPacket->type() != PACKET_TYPE_IDENTITY) {
            qCDebug(KDECONNECT_CORE) << "Received a UDP packet of wrong type" << receivedPacket->type();
            delete receivedPacket;
            continue;
        }

        if (receivedPacket->get<QString>(QStringLiteral("deviceId")) == KdeConnectConfig::instance().deviceId()) {
            // qCDebug(KDECONNECT_CORE) << "Ignoring my own broadcast";
            delete receivedPacket;
            continue;
        }

        int tcpPort = receivedPacket->get<int>(QStringLiteral("tcpPort"));
        if (tcpPort < MIN_TCP_PORT || tcpPort > MAX_TCP_PORT) {
            qCDebug(KDECONNECT_CORE) << "TCP port outside of kdeconnect's range";
            delete receivedPacket;
            continue;
        }

        // qCDebug(KDECONNECT_CORE) << "Received Udp identity packet from" << sender << " asking for a tcp connection on port " << tcpPort;

        if (m_receivedIdentityPackets.size() > MAX_REMEMBERED_IDENTITY_PACKETS) {
            qCWarning(KDECONNECT_CORE) << "Too many remembered identities, ignoring" << receivedPacket->get<QString>(QStringLiteral("deviceId"))
                                       << "received via UDP";
            delete receivedPacket;
            continue;
        }

        QSslSocket *socket = new QSslSocket(this);
        socket->setProxy(QNetworkProxy::NoProxy);
        m_receivedIdentityPackets[socket].np = receivedPacket;
        m_receivedIdentityPackets[socket].sender = sender;
        connect(socket, &QAbstractSocket::connected, this, &LanLinkProvider::tcpSocketConnected);
        connect(socket, &QAbstractSocket::errorOccurred, this, &LanLinkProvider::connectError);
        socket->connectToHost(sender, tcpPort);
    }
}

void LanLinkProvider::connectError(QAbstractSocket::SocketError socketError)
{
    QSslSocket *socket = qobject_cast<QSslSocket *>(sender());
    if (!socket)
        return;

    qCDebug(KDECONNECT_CORE) << "Socket error" << socketError;
    qCDebug(KDECONNECT_CORE) << "Fallback (1), try reverse connection (send udp packet)" << socket->errorString();
    NetworkPacket np = KdeConnectConfig::instance().deviceInfo().toIdentityPacket();
    np.set(QStringLiteral("tcpPort"), m_tcpPort);
    m_udpSocket.writeDatagram(np.serialize(), m_receivedIdentityPackets[socket].sender, m_udpBroadcastPort);

    // The socket we created didn't work, and we didn't manage
    // to create a LanDeviceLink from it, deleting everything.
    delete m_receivedIdentityPackets.take(socket).np;
    socket->deleteLater();
}

// We received a UDP packet and answered by connecting to them by TCP. This gets called on a successful connection.
void LanLinkProvider::tcpSocketConnected()
{
    QSslSocket *socket = qobject_cast<QSslSocket *>(sender());

    if (!socket) {
        return;
    }

    disconnect(socket, &QAbstractSocket::errorOccurred, this, &LanLinkProvider::connectError);

    configureSocket(socket);

    // If socket disconnects due to any reason after connection, link on ssl failure
    connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);

    NetworkPacket *receivedPacket = m_receivedIdentityPackets[socket].np;
    const QString &deviceId = receivedPacket->get<QString>(QStringLiteral("deviceId"));
    // qCDebug(KDECONNECT_CORE) << "tcpSocketConnected" << socket->isWritable();

    // If network is on ssl, do not believe when they are connected, believe when handshake is completed
    NetworkPacket np2 = KdeConnectConfig::instance().deviceInfo().toIdentityPacket();
    socket->write(np2.serialize());
    bool success = socket->waitForBytesWritten();

    if (success) {
        qCDebug(KDECONNECT_CORE) << "TCP connection done (i'm the existing device)";

        // if ssl supported
        bool isDeviceTrusted = KdeConnectConfig::instance().trustedDevices().contains(deviceId);
        configureSslSocket(socket, deviceId, isDeviceTrusted);

        qCDebug(KDECONNECT_CORE) << "Starting server ssl (I'm the client TCP socket)";

        connect(socket, &QSslSocket::encrypted, this, &LanLinkProvider::encrypted);

        connect(socket, &QSslSocket::sslErrors, this, &LanLinkProvider::sslErrors);

        socket->startServerEncryption();
    } else {
        // The socket doesn't seem to work, so we can't create the connection.

        qCDebug(KDECONNECT_CORE) << "Fallback (2), try reverse connection (send udp packet)";
        m_udpSocket.writeDatagram(np2.serialize(), m_receivedIdentityPackets[socket].sender, m_udpBroadcastPort);

        // Cleanup the network packet now. The socket should be deleted via the disconnected() signal.
        // We don't do this on success, because it is done later in the encrypted() slot.
        delete m_receivedIdentityPackets.take(socket).np;
    }
}

void LanLinkProvider::encrypted()
{
    qCDebug(KDECONNECT_CORE) << "Socket successfully established an SSL connection";

    QSslSocket *socket = qobject_cast<QSslSocket *>(sender());
    if (!socket)
        return;

    Q_ASSERT(socket->mode() != QSslSocket::UnencryptedMode);

    NetworkPacket *identityPacket = m_receivedIdentityPackets[socket].np;

    DeviceInfo deviceInfo = DeviceInfo::FromIdentityPacketAndCert(*identityPacket, socket->peerCertificate());

    addLink(socket, deviceInfo);

    // We don't delete the socket because now it's owned by the LanDeviceLink
    delete m_receivedIdentityPackets.take(socket).np;
}

void LanLinkProvider::sslErrors(const QList<QSslError> &errors)
{
    QSslSocket *socket = qobject_cast<QSslSocket *>(sender());
    if (!socket)
        return;

    bool fatal = false;
    for (const QSslError &error : errors) {
        if (error.error() != QSslError::SelfSignedCertificate) {
            qCCritical(KDECONNECT_CORE) << "Disconnecting due to fatal SSL Error: " << error;
            fatal = true;
        } else {
            qCDebug(KDECONNECT_CORE) << "Ignoring self-signed cert error";
        }
    }

    if (fatal) {
        socket->disconnectFromHost();
        delete m_receivedIdentityPackets.take(socket).np;
    }
}

// I'm the new device and this is the answer to my UDP identity packet (no data received yet). They are connecting to us through TCP, and they should send an
// identity.
void LanLinkProvider::newConnection()
{
    qCDebug(KDECONNECT_CORE) << "LanLinkProvider newConnection";

    while (m_server->hasPendingConnections()) {
        QSslSocket *socket = m_server->nextPendingConnection();
        configureSocket(socket);
        // This socket is still managed by us (and child of the QTcpServer), if
        // it disconnects before we manage to pass it to a LanDeviceLink, it's
        // our responsibility to delete it. We do so with this connection.
        connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QIODevice::readyRead, this, &LanLinkProvider::dataReceived);

        QTimer *timer = new QTimer(socket);
        timer->setSingleShot(true);
        timer->setInterval(1000);
        connect(socket, &QSslSocket::encrypted, timer, &QObject::deleteLater);
        connect(timer, &QTimer::timeout, socket, [socket] {
            qCWarning(KDECONNECT_CORE) << "LanLinkProvider/newConnection: Host timed out without sending any identity." << socket->peerAddress();
            socket->disconnectFromHost();
        });
        timer->start();
    }
}

// I'm the new device and this is the TCP response to my UDP identity packet
void LanLinkProvider::dataReceived()
{
    QSslSocket *socket = qobject_cast<QSslSocket *>(sender());
    // the size here is arbitrary and is now at 8192 bytes. It needs to be considerably long as it includes the capabilities but there needs to be a limit
    // Tested between my systems and I get around 2000 per identity package.
    if (socket->bytesAvailable() > 8192) {
        qCWarning(KDECONNECT_CORE) << "LanLinkProvider/newConnection: Suspiciously long identity package received. Closing connection." << socket->peerAddress()
                                   << socket->bytesAvailable();
        socket->disconnectFromHost();
        return;
    }

    if (!socket->canReadLine()) {
        // This can happen if the packet is large enough to be split in two chunks
        return;
    }

    const QByteArray data = socket->readLine();

    qCDebug(KDECONNECT_CORE) << "LanLinkProvider received reply:" << data;

    NetworkPacket *np = new NetworkPacket();
    bool success = NetworkPacket::unserialize(data, np);

    if (!success) {
        delete np;
        return;
    }

    if (np->type() != PACKET_TYPE_IDENTITY) {
        qCWarning(KDECONNECT_CORE) << "LanLinkProvider/newConnection: Expected identity, received " << np->type();
        delete np;
        return;
    }

    if (m_receivedIdentityPackets.size() > MAX_REMEMBERED_IDENTITY_PACKETS) {
        qCWarning(KDECONNECT_CORE) << "Too many remembered identities, ignoring" << np->get<QString>(QStringLiteral("deviceId")) << "received via TCP";
        delete np;
        return;
    }

    // Needed in "encrypted" if ssl is used, similar to "tcpSocketConnected"
    m_receivedIdentityPackets[socket].np = np;

    const QString &deviceId = np->get<QString>(QStringLiteral("deviceId"));
    // qCDebug(KDECONNECT_CORE) << "Handshaking done (i'm the new device)";

    // This socket will now be owned by the LanDeviceLink or we don't want more data to be received, forget about it
    disconnect(socket, &QIODevice::readyRead, this, &LanLinkProvider::dataReceived);

    bool isDeviceTrusted = KdeConnectConfig::instance().trustedDevices().contains(deviceId);
    configureSslSocket(socket, deviceId, isDeviceTrusted);

    qCDebug(KDECONNECT_CORE) << "Starting client ssl (but I'm the server TCP socket)";

    connect(socket, &QSslSocket::encrypted, this, &LanLinkProvider::encrypted);

    if (isDeviceTrusted) {
        connect(socket, &QSslSocket::sslErrors, this, &LanLinkProvider::sslErrors);
    }

    socket->startClientEncryption();
}

void LanLinkProvider::onLinkDestroyed(const QString &deviceId, DeviceLink *oldPtr)
{
    qCDebug(KDECONNECT_CORE) << "LanLinkProvider deviceLinkDestroyed" << deviceId;
    DeviceLink *link = m_links.take(deviceId);
    Q_ASSERT(link == oldPtr);
}

void LanLinkProvider::configureSslSocket(QSslSocket *socket, const QString &deviceId, bool isDeviceTrusted)
{
    // Configure for ssl
    QSslConfiguration sslConfig;
    sslConfig.setLocalCertificate(KdeConnectConfig::instance().certificate());

    QFile privateKeyFile(KdeConnectConfig::instance().privateKeyPath());
    QSslKey privateKey;
    if (privateKeyFile.open(QIODevice::ReadOnly)) {
        privateKey = QSslKey(privateKeyFile.readAll(), QSsl::Rsa);
    }
    privateKeyFile.close();
    sslConfig.setPrivateKey(privateKey);

    if (isDeviceTrusted) {
        QSslCertificate certificate = KdeConnectConfig::instance().getTrustedDeviceCertificate(deviceId);
        sslConfig.setCaCertificates({certificate});
        sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
    } else {
        sslConfig.setPeerVerifyMode(QSslSocket::QueryPeer);
    }
    socket->setSslConfiguration(sslConfig);
    socket->setPeerVerifyName(deviceId);

    // Usually SSL errors are only bad for trusted devices. Uncomment this section to log errors in any case, for debugging.
    // QObject::connect(socket, static_cast<void (QSslSocket::*)(const QList<QSslError>&)>(&QSslSocket::sslErrors), [](const QList<QSslError>& errors)
    //{
    //     Q_FOREACH (const QSslError& error, errors) {
    //         qCDebug(KDECONNECT_CORE) << "SSL Error:" << error.errorString();
    //     }
    // });
}

void LanLinkProvider::configureSocket(QSslSocket *socket)
{
    socket->setProxy(QNetworkProxy::NoProxy);

    socket->setSocketOption(QAbstractSocket::KeepAliveOption, QVariant(1));

#ifdef TCP_KEEPIDLE
    // time to start sending keepalive packets (seconds)
    int maxIdle = 10;
    setsockopt(socket->socketDescriptor(), IPPROTO_TCP, TCP_KEEPIDLE, &maxIdle, sizeof(maxIdle));
#endif

#ifdef TCP_KEEPINTVL
    // interval between keepalive packets after the initial period (seconds)
    int interval = 5;
    setsockopt(socket->socketDescriptor(), IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#endif

#ifdef TCP_KEEPCNT
    // number of missed keepalive packets before disconnecting
    int count = 3;
    setsockopt(socket->socketDescriptor(), IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif

#if defined(Q_OS_WIN)
    int maxIdle = 5 * 60 * 1000; // 5 minutes of idle before sending keep-alive
    int interval = 5 * 1000; // 5 seconds interval between probes after 5 minute delay
    DWORD nop;

    // see https://learn.microsoft.com/en-us/windows/win32/winsock/sio-keepalive-vals
    struct tcp_keepalive keepalive = {1 /* true */, maxIdle, interval};

    int rv = WSAIoctl(socket->socketDescriptor(), SIO_KEEPALIVE_VALS, &keepalive, sizeof(keepalive), nullptr, 0, &nop, nullptr, nullptr);
    if (!rv) {
        int error = WSAGetLastError();
        qCDebug(KDECONNECT_CORE) << "Could not enable TCP Keep-Alive: " << error;
    }
#endif
}

void LanLinkProvider::addLink(QSslSocket *socket, const DeviceInfo &deviceInfo)
{
    QString certDeviceId = socket->peerCertificate().subjectDisplayName();
    if (deviceInfo.id != certDeviceId) {
        socket->disconnectFromHost();
        qCWarning(KDECONNECT_CORE) << "DeviceID in cert doesn't match deviceID in identity packet. " << deviceInfo.id << " vs " << certDeviceId;
        return;
    }

    // Socket disconnection will now be handled by LanDeviceLink
    disconnect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);

    LanDeviceLink *deviceLink;
    // Do we have a link for this device already?
    QMap<QString, LanDeviceLink *>::iterator linkIterator = m_links.find(deviceInfo.id);
    if (linkIterator != m_links.end()) {
        deviceLink = linkIterator.value();
        if (deviceLink->deviceInfo().certificate != deviceInfo.certificate) {
            qWarning() << "LanLink was asked to replace a socket but the certificate doesn't match, aborting";
            return;
        }
        // qCDebug(KDECONNECT_CORE) << "Reusing link to" << deviceId;
        deviceLink->reset(socket);
    } else {
        deviceLink = new LanDeviceLink(deviceInfo, this, socket);
        // Socket disconnection will now be handled by LanDeviceLink
        disconnect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
        bool isDeviceTrusted = KdeConnectConfig::instance().trustedDevices().contains(deviceInfo.id);
        if (!isDeviceTrusted && m_links.size() > MAX_UNPAIRED_CONNECTIONS) {
            qCWarning(KDECONNECT_CORE) << "Too many unpaired devices to remember them all. Ignoring " << deviceInfo.id;
            socket->disconnectFromHost();
            socket->deleteLater();
            return;
        }
        m_links[deviceInfo.id] = deviceLink;
    }
    Q_EMIT onConnectionReceived(deviceLink);
}

#include "moc_lanlinkprovider.cpp"
