#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QCoreApplication>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow) {
    ui->setupUi(this);
    // 初始化所有变量
    m_tcpServer = nullptr;
    m_serverSocket = nullptr;
    m_serverFile = nullptr;
    m_clientSocket = nullptr;
    m_clientFile = nullptr;
    m_totalSize = 0;
    m_blockSize = 4 * 1024 * 1024; // 固定4MB块大小（大文件传输最优值）
    m_totalBlocks = 0;
    m_receivedBlocks = 0;
    m_currentBlock = 0;
    m_sentSize = 0;
    m_saveRootPath = "";
    m_totalFiles = 0;
    m_receivedFiles = 0;
    m_currentFileIndex = 0;
    m_isDirectoryTransfer = false;
    m_currentFileName = "";
    // 初始化UI状态
    ui->btnStopServer->setEnabled(false);
    ui->btnDisconnect->setEnabled(false);
    ui->btnSendFile->setEnabled(false);
    ui->progressBar->setValue(0);
}

MainWindow::~MainWindow() {
    // 安全释放所有资源
    if (m_tcpServer) {
        m_tcpServer->close();
        delete m_tcpServer;
    }
    if (m_serverSocket) {
        m_serverSocket->close();
        delete m_serverSocket;
    }
    if (m_serverFile) {
        m_serverFile->close();
        delete m_serverFile;
    }
    if (m_clientSocket) {
        m_clientSocket->close();
        delete m_clientSocket;
    }
    if (m_clientFile) {
        m_clientFile->close();
        delete m_clientFile;
    }
    delete ui;
}

// 添加带时间戳的日志
void MainWindow::appendLog(const QString &msg) {
    ui->textEditLog->append(QString("[%1] %2")
        .arg(QDateTime::currentDateTime().toString("hh:mm:ss"))
        .arg(msg));
}

/************************ 服务器端完整实现 ************************/
void MainWindow::on_btnStartServer_clicked() {
    quint16 port = ui->lineEditPort->text().toUShort();
    m_tcpServer = new QTcpServer(this);
    connect(m_tcpServer, &QTcpServer::newConnection, this, &MainWindow::onNewConnection);
    if (m_tcpServer->listen(QHostAddress::Any, port)) {
        appendLog(QString(" ✅  服务器启动成功，监听端口：%1").arg(port));
        ui->btnStartServer->setEnabled(false);
        ui->btnStopServer->setEnabled(true);
    } else {
        QMessageBox::critical(this, " ❌  错误", "服务器启动失败：" + m_tcpServer->errorString());
        delete m_tcpServer;
        m_tcpServer = nullptr;
    }
}

void MainWindow::on_btnStopServer_clicked() {
    if (m_tcpServer) {
        m_tcpServer->close();
        delete m_tcpServer;
        m_tcpServer = nullptr;
    }
    if (m_serverSocket) {
        m_serverSocket->close();
        delete m_serverSocket;
        m_serverSocket = nullptr;
    }
    appendLog(" ⏹️  服务器已停止");
    ui->btnStartServer->setEnabled(true);
    ui->btnStopServer->setEnabled(false);
}

// 有新客户端连接
void MainWindow::onNewConnection() {
    m_serverSocket = m_tcpServer->nextPendingConnection();
    // Qt 5兼容的Nagle算法禁用方式
    m_serverSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    appendLog(QString(" 🔌  新客户端连接：%1").arg(m_serverSocket->peerAddress().toString()));
    connect(m_serverSocket, &QTcpSocket::readyRead, this, &MainWindow::onServerReadyRead);
    connect(m_serverSocket, &QTcpSocket::disconnected, this, &MainWindow::onServerDisconnected);
    // Qt 5全版本兼容错误信号
    connect(m_serverSocket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this, [this](QAbstractSocket::SocketError) {
        appendLog(" ❌  服务器连接错误：" + m_serverSocket->errorString());
    });
    // 清空所有状态
    m_serverBuffer.clear();
    m_saveRootPath = "";
    m_totalFiles = 0;
    m_receivedFiles = 0;
    m_currentFileName = "";
}

//  ✅  非模态处理文件保存对话框，彻底解决阻塞问题
void MainWindow::handleFileSaveDialog(const QString& fileName, quint64 totalSize, quint32 blockSize, quint32 totalBlocks) {
    if (!m_serverSocket) return; // 连接已断开，直接返回
    QString savePath = QFileDialog::getSaveFileName(this, "保存文件", fileName);
    if (savePath.isEmpty()) {
        // 用户取消保存，发送错误信号
        PacketHeader errHeader;
        errHeader.type = 3;
        errHeader.totalSize = 0;
        errHeader.blockSize = 0;
        errHeader.blockIndex = 0;
        errHeader.totalBlocks = 0;
        errHeader.fileNameLen = 0;
        m_serverSocket->write(reinterpret_cast<char*>(&errHeader), sizeof(PacketHeader));
        m_serverSocket->flush();
        return;
    }
    // 打开文件并预分配空间
    m_serverFile = new QFile(savePath);
    if (!m_serverFile->open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, " ❌  错误", "无法打开文件：" + m_serverFile->errorString());
        delete m_serverFile;
        m_serverFile = nullptr;
        return;
    }
    m_serverFile->resize(static_cast<qint64>(totalSize));
    // 初始化分块传输参数
    m_totalSize = totalSize;
    m_blockSize = blockSize;
    m_totalBlocks = totalBlocks;
    m_receivedBlocks = 0;
    m_currentFileName = fileName;
    appendLog(QString(" 📥  开始接收文件：%1，大小：%2 MB，分%3块传输")
        .arg(fileName)
        .arg(totalSize / 1024.0 / 1024.0, 0, 'f', 2)
        .arg(totalBlocks));
    ui->progressBar->setMaximum(static_cast<int>(totalBlocks));
    ui->progressBar->setValue(0);
    // 发送第一个块的确认信号
    PacketHeader ackHeader;
    ackHeader.type = 4;
    ackHeader.totalSize = 0;
    ackHeader.blockSize = 0;
    ackHeader.blockIndex = 0;
    ackHeader.totalBlocks = 0;
    ackHeader.fileNameLen = 0;
    m_serverSocket->write(reinterpret_cast<char*>(&ackHeader), sizeof(PacketHeader));
    m_serverSocket->flush();
}

//  ✅  非模态处理目录保存对话框
void MainWindow::handleDirSaveDialog(const QString& dirName, quint32 totalFiles) {
    if (!m_serverSocket) return;
    QString saveRootPath = QFileDialog::getExistingDirectory(this, "选择保存目录");
    if (saveRootPath.isEmpty()) {
        // 用户取消保存
        PacketHeader errHeader;
        errHeader.type = 3;
        errHeader.totalSize = 0;
        errHeader.blockSize = 0;
        errHeader.blockIndex = 0;
        errHeader.totalBlocks = 0;
        errHeader.fileNameLen = 0;
        m_serverSocket->write(reinterpret_cast<char*>(&errHeader), sizeof(PacketHeader));
        m_serverSocket->flush();
        return;
    }
    // 拼接完整的保存路径
    m_saveRootPath = saveRootPath + QDir::separator() + dirName;
    QDir().mkpath(m_saveRootPath); // 创建根目录
    m_totalFiles = totalFiles;
    m_receivedFiles = 0;
    appendLog(QString(" 📂  开始接收目录：%1，共%2个文件").arg(dirName).arg(totalFiles));
    ui->progressBar->setMaximum(static_cast<int>(totalFiles));
    ui->progressBar->setValue(0);
    // 发送确认信号，通知客户端开始发送第一个文件
    PacketHeader ackHeader;
    ackHeader.type = 4;
    ackHeader.totalSize = 0;
    ackHeader.blockSize = 0;
    ackHeader.blockIndex = 0xFFFFFFFF; // 特殊标记：目录开始确认
    ackHeader.totalBlocks = 0;
    ackHeader.fileNameLen = 0;
    m_serverSocket->write(reinterpret_cast<char*>(&ackHeader), sizeof(PacketHeader));
    m_serverSocket->flush();
}

// 服务器端带缓冲区的接收逻辑
void MainWindow::onServerReadyRead() {
    // 1. 把所有收到的数据追加到缓冲区
    m_serverBuffer.append(m_serverSocket->readAll());
    // 2. 循环处理缓冲区中的完整数据包
    while (true) {
        // 检查缓冲区是否有足够的数据解析一个完整的协议头
        if (static_cast<quint32>(m_serverBuffer.size()) < sizeof(PacketHeader)) {
            break; // 数据不够，等待下一次接收
        }
        // 3. 解析协议头
        PacketHeader header;
        memcpy(&header, m_serverBuffer.constData(), sizeof(PacketHeader));
        // 4. 检查是否有足够的数据处理这个包
        int packetSize = sizeof(PacketHeader);
        if (header.type == 0 || header.type == 5) { // 文件信息包和目录开始包后面跟着文件名
            packetSize += static_cast<int>(header.fileNameLen);
        } else if (header.type == 1) { // 文件块后面跟着块数据
            packetSize += static_cast<int>(header.blockSize);
        }
        if (static_cast<quint32>(m_serverBuffer.size()) < static_cast<quint32>(packetSize)) {
            break; // 这个包还没接收完整，等待下一次
        }
        // 5. 处理不同类型的包
        if (header.type == 0) { // 收到文件信息包
            // 从缓冲区读取文件名
            QByteArray fileNameData = m_serverBuffer.mid(sizeof(PacketHeader), static_cast<int>(header.fileNameLen));
            QString fileName = QString::fromUtf8(fileNameData);
            // 从缓冲区移除这个已处理的包
            m_serverBuffer = m_serverBuffer.mid(packetSize);

            // 【修复漏洞 1：判断是否处于目录传输模式】
            if (!m_saveRootPath.isEmpty()) {
                // 强制将 Windows 的反斜杠 \ 转换为跨平台正斜杠 /
                fileName.replace("\\", "/");
                QString fullPath = m_saveRootPath + "/" + fileName;

                // 自动创建多级子目录（防止接收端找不到路径而崩溃）
                QFileInfo info(fullPath);
                QDir().mkpath(info.absolutePath());

                // 静默打开文件，不弹窗
                m_serverFile = new QFile(fullPath);
                if (m_serverFile->open(QIODevice::WriteOnly)) {
                    m_serverFile->resize(static_cast<qint64>(header.totalSize));
                    m_totalSize = header.totalSize;
                    m_blockSize = header.blockSize;
                    m_totalBlocks = header.totalBlocks;
                    m_receivedBlocks = 0;
                    m_currentFileName = fileName;

                    // 发送该文件的第一块请求 ACK
                    PacketHeader ackHeader;
                    ackHeader.type = 4;
                    ackHeader.totalSize = 0;
                    ackHeader.blockSize = 0;
                    ackHeader.blockIndex = 0;
                    ackHeader.totalBlocks = 0;
                    ackHeader.fileNameLen = 0;
                    m_serverSocket->write(reinterpret_cast<char*>(&ackHeader), sizeof(PacketHeader));
                    m_serverSocket->flush();
                } else {
                    appendLog(" ❌ 无法创建子文件：" + fullPath);
                }
            } else {
                // 单文件传输，正常弹出保存对话框
                QMetaObject::invokeMethod(this, [this, fileName, header]() {
                    handleFileSaveDialog(fileName, header.totalSize, header.blockSize, header.totalBlocks);
                }, Qt::QueuedConnection);
                return; // 只有单文件弹窗时才需要 return 挂起事件循环
            }
        }
        else if (header.type == 1) { // 收到文件块
            if (!m_serverFile) {
                m_serverBuffer = m_serverBuffer.mid(packetSize);
                break;
            }
            // 从缓冲区读取块数据
            QByteArray blockData = m_serverBuffer.mid(sizeof(PacketHeader), static_cast<int>(header.blockSize));
            // 写入到正确的位置
            m_serverFile->seek(static_cast<qint64>(header.blockIndex) * static_cast<qint64>(m_blockSize));
            m_serverFile->write(blockData);
            m_receivedBlocks++;

            // 如果是单文件传输，进度条展示块进度
            if (m_saveRootPath.isEmpty()) {
                ui->progressBar->setValue(static_cast<int>(m_receivedBlocks));
            }

            // 检查是否所有块都接收完成
            if (m_receivedBlocks == m_totalBlocks) {
                m_serverFile->close();
                delete m_serverFile;
                m_serverFile = nullptr;
                appendLog(QString(" ✅  文件接收完成：%1").arg(m_currentFileName));

                // 【细节优化：如果是目录传输，不弹出单个文件的成功提示，转为更新整体文件进度】
                if (m_saveRootPath.isEmpty()) {
                    QMessageBox::information(this, "成功", "文件接收完成！");
                } else {
                    m_receivedFiles++;
                    ui->progressBar->setValue(static_cast<int>(m_receivedFiles));
                }

                // 发送最终完成信号
                PacketHeader finishHeader;
                finishHeader.type = 2;
                finishHeader.totalSize = 0;
                finishHeader.blockSize = 0;
                finishHeader.blockIndex = 0;
                finishHeader.totalBlocks = 0;
                finishHeader.fileNameLen = 0;
                m_serverSocket->write(reinterpret_cast<char*>(&finishHeader), sizeof(PacketHeader));
                m_serverSocket->flush();
            } else {
                // 发送下一个块的确认信号
                PacketHeader ackHeader;
                ackHeader.type = 4;
                ackHeader.totalSize = 0;
                ackHeader.blockSize = 0;
                ackHeader.blockIndex = header.blockIndex + 1;
                ackHeader.totalBlocks = 0;
                ackHeader.fileNameLen = 0;
                m_serverSocket->write(reinterpret_cast<char*>(&ackHeader), sizeof(PacketHeader));
                m_serverSocket->flush();
            }
            // 从缓冲区中移除已处理的块包
            m_serverBuffer = m_serverBuffer.mid(packetSize);
        }
        else if (header.type == 5) { // 收到目录开始信号
            QByteArray dirNameData = m_serverBuffer.mid(sizeof(PacketHeader), static_cast<int>(header.fileNameLen));
            QString dirName = QString::fromUtf8(dirNameData);
            // 从缓冲区移除这个包
            m_serverBuffer = m_serverBuffer.mid(packetSize);
            //  ✅  延迟调用目录保存对话框
            QMetaObject::invokeMethod(this, [this, dirName, header]() {
                handleDirSaveDialog(dirName, static_cast<quint32>(header.totalSize));
            }, Qt::QueuedConnection);
            return;
        }
        else if (header.type == 6) { // 收到目录结束信号
            appendLog(QString(" 🎉  目录接收完成！共成功接收%1个文件").arg(m_receivedFiles));
            QMessageBox::information(this, "成功", QString("目录接收完成！\n共成功接收%1个文件").arg(m_receivedFiles));
            // 重置目录传输状态
            m_saveRootPath = "";
            m_totalFiles = 0;
            m_receivedFiles = 0;
            m_currentFileName = "";
            m_serverBuffer = m_serverBuffer.mid(packetSize);
        }
    }
}

// 客户端断开连接
void MainWindow::onServerDisconnected() {
    appendLog(" 🔌  客户端已断开连接");
    m_serverSocket->deleteLater();
    m_serverSocket = nullptr;
    // 如果正在接收文件，清理资源
    if (m_serverFile) {
        m_serverFile->close();
        delete m_serverFile;
        m_serverFile = nullptr;
        appendLog(" ⚠️  文件接收中断");
    }
    ui->progressBar->setValue(0);
    m_serverBuffer.clear();
    m_saveRootPath = "";
    m_totalFiles = 0;
    m_receivedFiles = 0;
    m_currentFileName = "";
}

/************************ 客户端完整实现 ************************/
void MainWindow::on_btnConnect_clicked() {
    QString ip = ui->lineEditIP->text();
    quint16 port = ui->lineEditClientPort->text().toUShort();
    m_clientSocket = new QTcpSocket(this);
    // Qt 5兼容的Nagle算法禁用方式
    m_clientSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(m_clientSocket, &QTcpSocket::connected, this, &MainWindow::onClientConnected);
    connect(m_clientSocket, &QTcpSocket::readyRead, this, &MainWindow::onClientReadyRead);
    connect(m_clientSocket, &QTcpSocket::disconnected, this, &MainWindow::onClientDisconnected);
    connect(m_clientSocket, &QTcpSocket::bytesWritten, this, &MainWindow::onBytesWritten);
    // Qt 5全版本兼容错误信号
    connect(m_clientSocket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this, [this](QAbstractSocket::SocketError) {
        appendLog(" ❌  客户端错误：" + m_clientSocket->errorString());
    });
    m_clientSocket->connectToHost(ip, port);
    appendLog(QString(" 🔗  正在连接服务器：%1:%2").arg(ip).arg(port));
    // 清空所有状态
    m_clientBuffer.clear();
    m_fileList.clear();
    m_currentFileIndex = 0;
    m_isDirectoryTransfer = false;
}

void MainWindow::on_btnDisconnect_clicked() {
    if (m_clientSocket) {
        m_clientSocket->disconnectFromHost();
    }
}

// 连接服务器成功
void MainWindow::onClientConnected() {
    appendLog(" ✅  连接服务器成功");
    ui->btnConnect->setEnabled(false);
    ui->btnDisconnect->setEnabled(true);
    ui->btnSendFile->setEnabled(true);
}

// 客户端带缓冲区的接收逻辑
void MainWindow::onClientReadyRead() {
    // 1. 把所有收到的数据追加到缓冲区
    m_clientBuffer.append(m_clientSocket->readAll());
    // 2. 循环处理缓冲区中的完整数据包
    while (true) {
        // 检查缓冲区是否有足够的数据解析一个完整的协议头
        if (static_cast<quint32>(m_clientBuffer.size()) < sizeof(PacketHeader)) {
            break; // 数据不够，等待下一次接收
        }
        // 3. 解析协议头
        PacketHeader header;
        memcpy(&header, m_clientBuffer.constData(), sizeof(PacketHeader));
        // 4. 从缓冲区中移除这个完整的包头
        m_clientBuffer = m_clientBuffer.mid(sizeof(PacketHeader));
        // 5. 处理不同类型的包
        if (header.type == 4) { // 收到确认信号
            if (header.blockIndex == 0xFFFFFFFF) { // 特殊标记：目录开始确认
                appendLog(" 📩  收到服务器目录开始确认，开始发送第一个文件");
                sendNextFile(); // 发送第一个文件
            } else if (m_clientFile != nullptr) { // 文件块确认
                m_currentBlock = header.blockIndex;
                sendNextBlock(); // 发送下一个块
            }
        } else if (header.type == 2) { // 收到文件传输完成信号
            if (m_isDirectoryTransfer) {
                // 目录传输：一个文件完成，发送下一个
                m_clientFile->close();
                delete m_clientFile;
                m_clientFile = nullptr;
                appendLog(QString(" ✅  第%1/%2个文件发送完成").arg(m_currentFileIndex + 1).arg(m_fileList.size()));
                m_currentFileIndex++;
                sendNextFile();
            } else {
                // 单个文件传输完成
                appendLog(" ✅  文件发送完成");
                QMessageBox::information(this, "成功", "文件发送完成！");
                // 清理资源
                m_clientFile->close();
                delete m_clientFile;
                m_clientFile = nullptr;
                ui->progressBar->setValue(0);
            }
        } else if (header.type == 3) { // 收到错误信号
            if (m_isDirectoryTransfer) {
                if (m_clientFile) {
                    m_clientFile->close();
                    delete m_clientFile;
                    m_clientFile = nullptr;
                }
                appendLog(QString(" ❌  第%1/%2个文件发送失败，跳过").arg(m_currentFileIndex + 1).arg(m_fileList.size()));
                m_currentFileIndex++;
                sendNextFile();
            } else {
                appendLog(" ❌  服务器拒绝接收文件");
                if (m_clientFile) {
                    m_clientFile->close();
                    delete m_clientFile;
                    m_clientFile = nullptr;
                }
                ui->progressBar->setValue(0);
            }
        }
    }
}

// 断开连接
void MainWindow::onClientDisconnected() {
    appendLog(" 🔌  与服务器断开连接");
    m_clientSocket->deleteLater();
    m_clientSocket = nullptr;
    ui->btnConnect->setEnabled(true);
    ui->btnDisconnect->setEnabled(false);
    ui->btnSendFile->setEnabled(false);
    // 如果正在发送文件，清理资源
    if (m_clientFile) {
        m_clientFile->close();
        delete m_clientFile;
        m_clientFile = nullptr;
        appendLog(" ⚠️  文件发送中断");
    }
    ui->progressBar->setValue(0);
    m_clientBuffer.clear();
    m_fileList.clear();
    m_currentFileIndex = 0;
    m_isDirectoryTransfer = false;
}

// 数据已写入socket
void MainWindow::onBytesWritten(qint64 bytes) {
    Q_UNUSED(bytes);
}

// 分块传输核心：发送下一个 file block
void MainWindow::sendNextBlock() {
    if (m_clientFile == nullptr) return;
    // 所有块发送完成
    if (m_currentBlock >= m_totalBlocks) {
        return;
    }
    // 定位到当前块的起始位置
    m_clientFile->seek(static_cast<qint64>(m_currentBlock) * static_cast<qint64>(m_blockSize));
    QByteArray blockData = m_clientFile->read(static_cast<qint64>(m_blockSize));
    // 发送块头和块数据（补全所有结构体字段）
    PacketHeader blockHeader;
    blockHeader.type = 1;
    blockHeader.totalSize = m_totalSize;
    blockHeader.blockSize = static_cast<quint32>(blockData.size());
    blockHeader.blockIndex = m_currentBlock;
    blockHeader.totalBlocks = m_totalBlocks;
    blockHeader.fileNameLen = 0;
    m_clientSocket->write(reinterpret_cast<char*>(&blockHeader), sizeof(PacketHeader));
    m_clientSocket->write(blockData);
    m_clientSocket->flush(); // 强制立即发送
    // 更新进度条（目录传输时更新的是文件数进度，这里无需更新块进度）
    if (!m_isDirectoryTransfer) {
        ui->progressBar->setValue(static_cast<int>(m_currentBlock + 1));
    }
}

/************************ 文件传输控制 ************************/
void MainWindow::on_btnSelectFile_clicked() {
    QString filePath = QFileDialog::getOpenFileName(this, "选择文件");
    if (!filePath.isEmpty()) {
        ui->lineEditFilePath->setText(filePath);
    }
}

// 发送文件（自动识别文件和目录）
void MainWindow::on_btnSendFile_clicked() {
    QString path = ui->lineEditFilePath->text();
    if (path.isEmpty()) {
        QMessageBox::warning(this, " ⚠️  警告", "请先选择文件或目录");
        return;
    }
    QFileInfo fileInfo(path);
    if (fileInfo.isDir()) {
        // 如果是目录，开始目录传输
        m_fileList.clear();
        traverseDirectory(path, path, "",  m_fileList );
        if (m_fileList.isEmpty()) {
            QMessageBox::warning(this, " ⚠️  警告", "所选目录为空");
            return;
        }
        m_isDirectoryTransfer = true;
        m_currentFileIndex = 0;
        m_clientFile = nullptr;
        appendLog(QString(" 📂  准备发送目录：%1，共%2个文件").arg(fileInfo.fileName()).arg(m_fileList.size()));
        ui->progressBar->setMaximum(m_fileList.size());
        ui->progressBar->setValue(0);
        // 发送目录开始信号（补全所有结构体字段）
        PacketHeader dirHeader;
        dirHeader.type = 5;
        dirHeader.totalSize = static_cast<quint64>(m_fileList.size());
        dirHeader.blockSize = 0;
        dirHeader.blockIndex = 0;
        dirHeader.totalBlocks = 0;
        QByteArray dirNameData = fileInfo.fileName().toUtf8();
        dirHeader.fileNameLen = static_cast<quint32>(dirNameData.size());
        m_clientSocket->write(reinterpret_cast<char*>(&dirHeader), sizeof(PacketHeader));
        m_clientSocket->write(dirNameData);
        m_clientSocket->flush();
    } else {
        // 如果是文件，走原来的文件发送逻辑
        m_isDirectoryTransfer = false;
        m_clientFile = new QFile(path);
        if (!m_clientFile->open(QIODevice::ReadOnly)) {
            QMessageBox::critical(this, " ❌  错误", "无法打开文件：" + m_clientFile->errorString());
            delete m_clientFile;
            m_clientFile = nullptr;
            return;
        }
        // 获取文件信息并计算分块参数
        m_totalSize = static_cast<quint64>(fileInfo.size());
        m_currentBlock = 0;
        m_totalBlocks = static_cast<quint32>((m_totalSize + m_blockSize - 1) / m_blockSize);
        // 准备并发送文件信息头（补全所有结构体字段）
        PacketHeader header;
        header.type = 0;
        header.totalSize = m_totalSize;
        header.blockSize = m_blockSize;
        header.blockIndex = 0;
        header.totalBlocks = m_totalBlocks;
        QByteArray fileNameData = fileInfo.fileName().toUtf8();
        header.fileNameLen = static_cast<quint32>(fileNameData.size());
        m_clientSocket->write(reinterpret_cast<char*>(&header), sizeof(PacketHeader));
        m_clientSocket->write(fileNameData);
        m_clientSocket->flush();
        appendLog(QString(" 📤  准备发送文件：%1，大小：%2 MB，分%3块传输")
            .arg(fileInfo.fileName())
            .arg(m_totalSize / 1024.0 / 1024.0, 0, 'f', 2)
            .arg(m_totalBlocks));
        ui->progressBar->setMaximum(static_cast<int>(m_totalBlocks));
        ui->progressBar->setValue(0);
    }
}

/************************ 目录传输核心实现 ************************/
// 递归遍历目录，获取所有文件及其相对路径
void MainWindow::traverseDirectory(const QString &rootDir, const QString &currentDir,
                                   const QString &relativePath, QList<QPair<QString, QString>> &fileList) {
    QDir dir(currentDir);
    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (entry.isDir()) {
            // 递归遍历子目录
            traverseDirectory(rootDir, entry.filePath(), relativePath + entry.fileName() + QDir::separator(),  fileList );
        } else {
            // 保存文件的绝对路径和相对于根目录的路径
            fileList.append(qMakePair(entry.filePath(), relativePath + entry.fileName()));
        }
    }
}

// 目录传输：发送下一个文件
void MainWindow::sendNextFile() {
    // 所有文件发送完成
    if (m_currentFileIndex >= static_cast<quint32>(m_fileList.size())) {
        // 发送目录结束信号（补全所有结构体字段）
        PacketHeader finishHeader;
        finishHeader.type = 6;
        finishHeader.totalSize = 0;
        finishHeader.blockSize = 0;
        finishHeader.blockIndex = 0;
        finishHeader.totalBlocks = 0;
        finishHeader.fileNameLen = 0;
        m_clientSocket->write(reinterpret_cast<char*>(&finishHeader), sizeof(PacketHeader));
        m_clientSocket->flush();
        appendLog(" 🎉  目录发送完成！");
        QMessageBox::information(this, "成功", "目录发送完成！");
        // 清理资源
        m_fileList.clear();
        m_currentFileIndex = 0;
        m_isDirectoryTransfer = false;
        ui->progressBar->setValue(0);
        return;
    }
    // 获取当前要发送的文件
    QPair<QString, QString> filePair = m_fileList.at(static_cast<int>(m_currentFileIndex));
    QString absolutePath = filePair.first;
    QString relativePath = filePair.second;
    // 打开文件
    m_clientFile = new QFile(absolutePath);
    if (!m_clientFile->open(QIODevice::ReadOnly)) {
        appendLog(QString(" ⚠️  无法打开文件：%1，跳过").arg(absolutePath));
        delete m_clientFile;
        m_clientFile = nullptr;
        m_currentFileIndex++;
        sendNextFile();
        return;
    }
    // 获取文件信息并计算分块参数
    QFileInfo fileInfo(absolutePath);
    m_totalSize = static_cast<quint64>(fileInfo.size());
    m_currentBlock = 0;
    m_totalBlocks = static_cast<quint32>((m_totalSize + m_blockSize - 1) / m_blockSize);
    // 准备并发送文件信息头（补全所有结构体字段）
    PacketHeader header;
    header.type = 0;
    header.totalSize = m_totalSize;
    header.blockSize = m_blockSize;
    header.blockIndex = 0;
    header.totalBlocks = m_totalBlocks;

    // 【修复漏洞 2：清洗发送端路径，消除 Windows 的反斜杠 \ 对 Linux 系统引发的创建灾难】
    QString networkPath = relativePath;
    networkPath.replace("\\", "/");
    QByteArray fileNameData = networkPath.toUtf8();

    header.fileNameLen = static_cast<quint32>(fileNameData.size());
    m_clientSocket->write(reinterpret_cast<char*>(&header), sizeof(PacketHeader));
    m_clientSocket->write(fileNameData);
    m_clientSocket->flush();
    appendLog(QString(" 📤  正在发送第%1/%2个文件：%3")
              .arg(m_currentFileIndex + 1)
              .arg(m_fileList.size())
              .arg(networkPath));
    ui->progressBar->setValue(static_cast<int>(m_currentFileIndex));
}

// 选择目录按钮点击事件
void MainWindow::on_btnSelectDir_clicked() {
    QString dirPath = QFileDialog::getExistingDirectory(this, "选择要发送的目录");
    if (!dirPath.isEmpty()) {
        ui->lineEditFilePath->setText(dirPath);
    }
}
