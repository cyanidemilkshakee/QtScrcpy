#ifndef ADBSUITEDIALOG_H
#define ADBSUITEDIALOG_H

#include <QDialog>
#include <QProcess>
#include <QStringList>

#include <functional>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;
class QTabWidget;

class AdbSuiteDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AdbSuiteDialog(const QString &adbPath, const QStringList &devices,
                            const QString &selectedDevice, QWidget *parent = nullptr);
    ~AdbSuiteDialog() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    typedef std::function<void(int, const QString &, const QString &)> Completion;

    void buildUi();
    QWidget *buildDevicesPage();
    QWidget *buildAppsPage();
    QWidget *buildFilesPage();
    QWidget *buildWirelessPage();
    QWidget *buildDiagnosticsPage();
    QWidget *buildShellPage();
    QWidget *buildWorkflowsPage();
    void runAdb(const QStringList &arguments, const QString &description,
                const Completion &completion = Completion(), bool useTarget = true,
                const QString &targetOverride = QString());
    void appendOutput(const QString &text);
    QString selectedDevice() const;
    void refreshDevices();
    void refreshPackages();
    void filterPackages(const QString &filter);
    void inspectSelectedApp();
    void startLogcat();
    void stopLogcat();
    void saveLogcat();
    void captureBugReport();
    void installApk();
    void launchSelectedApp();
    void uninstallSelectedApp();
    void exportSelectedApk();
    void pushFile();
    void pullFile();
    void pairWirelessDevice();
    void connectWirelessDevice();
    void runShellCommand();
    void createPortRule();
    void listPortRules();
    void removePortRule();
    void saveWorkflow();
    void loadWorkflow();
    void runWorkflow();
    void runWorkflowStep(const QStringList &targets, const QStringList &commands,
                         int targetIndex, int commandIndex);
    QStringList workflowCommands() const;

private:
    QString m_adbPath;
    QString m_commandOutput;
    QString m_errorOutput;
    QString m_operationDescription;
    QString m_logcatPending;
    QStringList m_onlineDevices;
    Completion m_completion;
    bool m_commandCompleted = true;

    QProcess m_commandProcess;
    QProcess m_logcatProcess;
    QComboBox *m_deviceBox = nullptr;
    QTableWidget *m_deviceTable = nullptr;
    QListWidget *m_packageList = nullptr;
    QCheckBox *m_includeSystemApps = nullptr;
    QLineEdit *m_packageFilter = nullptr;
    QPlainTextEdit *m_console = nullptr;
    QPlainTextEdit *m_logcatView = nullptr;
    QLineEdit *m_pushLocalPath = nullptr;
    QLineEdit *m_pushRemotePath = nullptr;
    QLineEdit *m_pullRemotePath = nullptr;
    QLineEdit *m_pullLocalPath = nullptr;
    QLineEdit *m_wirelessHost = nullptr;
    QSpinBox *m_pairPort = nullptr;
    QSpinBox *m_connectPort = nullptr;
    QLineEdit *m_pairCode = nullptr;
    QLineEdit *m_logcatFilter = nullptr;
    QLineEdit *m_shellCommand = nullptr;
    QComboBox *m_portDirection = nullptr;
    QSpinBox *m_hostPort = nullptr;
    QSpinBox *m_devicePort = nullptr;
    QPlainTextEdit *m_workflowEditor = nullptr;
    QCheckBox *m_runWorkflowOnAll = nullptr;
    QTabWidget *m_tabs = nullptr;
};

#endif // ADBSUITEDIALOG_H
