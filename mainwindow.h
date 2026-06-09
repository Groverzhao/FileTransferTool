#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QDateTime>
#include <QByteArray>
#include <QDir>
#include <QMetaObject>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

// ✅ 跨平台终极对齐方案：同时支持MSVC和GCC
#pragma pack(1)
struct PacketHeader {
    quint32 type;        // 0=文件信息, 1=文件块, 2=传输完成, 3=错误, 4=块确认, 5=目录开始, 6=目录结束
    quint64 totalSize;   // 文件总大小/目录总文件数
    quint32 blockSize;   // 每个块的大小（4MB）
    quint32 blockIndex;  // 当前块索引
    quint32 totalBlocks; // 总块数
    quint32 fileNameLen; // 文件名长度
} __attribute__((packed)); // GCC强制1字节对齐
#pragma pack()

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // 服务器相关槽函数
    void on_btnStartServer_clicked();
    void on_btnStopServer_clicked();
    void onNewConnection();
    void onServerReadyRead();
    void onServerDisconnected();

    // 客户端相关槽函数
    void on_btnConnect_clicked();
    void on_btnDisconnect_clicked();
    void onClientConnected();
    void onClientReadyRead();
    void onClientDisconnected();
    void onBytesWritten(qint64 bytes);

    // 文件传输相关槽函数
    void on_btnSelectFile_clicked();
    void on_btnSendFile_clicked();
    void sendNextBlock(); // 分块传输：发送下一个文件块

    // 目录传输新增槽函数
    void on_btnSelectDir_clicked();
    void sendNextFile();  // 目录传输：发送下一个文件

    // ✅ 新增：非模态处理保存对话框，解决阻塞问题
    void handleFileSaveDialog(const QString& fileName, quint64 totalSize, quint32 blockSize, quint32 totalBlocks);
    void handleDirSaveDialog(const QString& dirName, quint32 totalFiles);

private:
    Ui::MainWindow *ui;
    QString m_currentFileName;

    // 服务器相关变量
    QTcpServer *m_tcpServer;
    QTcpSocket *m_serverSocket; // 服务器端用于和客户端通信的socket
    QFile *m_serverFile;        // 服务器端接收的文件
    quint64 m_totalSize;        // 文件总大小
    quint32 m_blockSize;        // 分块传输：每个块的大小（4MB）
    quint32 m_totalBlocks;      // 分块传输：总块数
    quint32 m_receivedBlocks;   // 分块传输：已接收块数
    QString m_saveFileName;     // 保存的文件名
    QByteArray m_serverBuffer;  // 服务器端接收缓冲区，彻底解决粘包
    QString m_saveRootPath;     // 目录传输：保存的根目录
    quint32 m_totalFiles;       // 目录传输：总文件数
    quint32 m_receivedFiles;    // 目录传输：已接收文件数

    // 客户端相关变量
    QTcpSocket *m_clientSocket; // 客户端socket
    QFile *m_clientFile;        // 客户端要发送的文件
    quint32 m_currentBlock;     // 分块传输：当前发送的块索引
    quint64 m_sentSize;         // 已发送大小
    QByteArray m_clientBuffer;  // 客户端接收缓冲区，彻底解决粘包
    QList<QPair<QString, QString>> m_fileList; // 目录传输：待发送的文件列表(绝对路径, 相对路径)
    quint32 m_currentFileIndex; // 目录传输：当前发送的文件索引
    bool m_isDirectoryTransfer; // 目录传输：是否正在传输目录

    // 辅助函数
    void appendLog(const QString &msg); // 添加日志
    static void traverseDirectory(const QString &rootDir, const QString &currentDir,
                                  const QString &relativePath, QList<QPair<QString, QString>> &fileList);
};

#endif // MAINWINDOW_H
