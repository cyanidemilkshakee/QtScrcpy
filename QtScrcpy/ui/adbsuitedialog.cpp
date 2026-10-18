#include "adbsuitedialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QAbstractItemView>
#include <QCloseEvent>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

AdbSuiteDialog::AdbSuiteDialog(const QString &adbPath, const QStringList &devices,
                               const QString &selectedDevice, QWidget *parent)
    : QDialog(parent), m_adbPath(adbPath), m_onlineDevices(devices)
{
    if (m_adbPath.isEmpty()) {
        m_adbPath = QStandardPaths::findExecutable(
#ifdef Q_OS_WIN
                    QStringLiteral("adb.exe")
#else
                    QStringLiteral("adb")
#endif
                    );
    }
    if (m_adbPath.isEmpty()) {
#ifdef Q_OS_WIN
        m_adbPath = QStringLiteral("adb.exe");
#else
        m_adbPath = QStringLiteral("adb");
#endif
    }

    setWindowTitle(tr("QtScrcpy ADB Suite"));
    resize(1020, 760);
    buildUi();

    for (const QString &serial : devices) {
        if (!serial.trimmed().isEmpty()) m_deviceBox->addItem(serial, serial);
    }
    int selectedIndex = m_deviceBox->findData(selectedDevice);
    if (selectedIndex >= 0) m_deviceBox->setCurrentIndex(selectedIndex);

    connect(&m_commandProcess, &QProcess::readyReadStandardOutput, this, [this] {
        m_commandOutput += QString::fromUtf8(m_commandProcess.readAllStandardOutput());
    });
    connect(&m_commandProcess, &QProcess::readyReadStandardError, this, [this] {
        m_errorOutput += QString::fromUtf8(m_commandProcess.readAllStandardError());
    });
    connect(&m_commandProcess, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (m_commandCompleted) return;
        m_commandCompleted = true;
        const int effectiveExitCode = exitStatus == QProcess::NormalExit ? exitCode : -1;
        const QString output = m_commandOutput.trimmed();
        const QString error = m_errorOutput.trimmed();
        appendOutput(tr("Finished: %1 (exit code %2)").arg(m_operationDescription).arg(effectiveExitCode));
        if (!output.isEmpty()) appendOutput(output);
        if (!error.isEmpty()) appendOutput(error);
        const Completion completion = m_completion;
        m_completion = Completion();
        if (completion) completion(effectiveExitCode, output, error);
    });
    connect(&m_commandProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_commandCompleted) return;
        m_commandCompleted = true;
        const QString message = tr("Could not start ADB at %1: %2")
                                    .arg(m_adbPath, m_commandProcess.errorString());
        appendOutput(message);
        const Completion completion = m_completion;
        m_completion = Completion();
        if (completion) completion(-1, QString(), message);
    });

    connect(&m_logcatProcess, &QProcess::readyReadStandardOutput, this, [this] {
        m_logcatPending += QString::fromUtf8(m_logcatProcess.readAllStandardOutput());
        const QStringList lines = m_logcatPending.split(QRegularExpression("\\r?\\n"));
        if (lines.isEmpty()) return;
        m_logcatPending = lines.last();
        const QString filter = m_logcatFilter->text().trimmed();
        for (int i = 0; i + 1 < lines.size(); ++i) {
            if (filter.isEmpty() || lines.at(i).contains(filter, Qt::CaseInsensitive)) {
                m_logcatView->appendPlainText(lines.at(i));
            }
        }
    });
    connect(&m_logcatProcess, &QProcess::readyReadStandardError, this, [this] {
        const QString error = QString::fromUtf8(m_logcatProcess.readAllStandardError()).trimmed();
        if (!error.isEmpty()) appendOutput(error);
    });
    connect(&m_logcatProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            appendOutput(tr("Could not start Logcat: %1").arg(m_logcatProcess.errorString()));
            m_deviceBox->setEnabled(true);
        }
    });
    connect(&m_logcatProcess, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
        m_deviceBox->setEnabled(true);
        if (!m_logcatPending.trimmed().isEmpty()) {
            const QString filter = m_logcatFilter->text().trimmed();
            if (filter.isEmpty() || m_logcatPending.contains(filter, Qt::CaseInsensitive)) {
                m_logcatView->appendPlainText(m_logcatPending);
            }
            m_logcatPending.clear();
        }
        appendOutput(tr("Logcat process ended."));
    });

    refreshDevices();
}

AdbSuiteDialog::~AdbSuiteDialog()
{
    stopLogcat();
}

void AdbSuiteDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    auto *targetRow = new QHBoxLayout();
    targetRow->addWidget(new QLabel(tr("Target device:"), this));
    m_deviceBox = new QComboBox(this);
    m_deviceBox->setMinimumWidth(280);
    targetRow->addWidget(m_deviceBox, 1);
    auto *refreshButton = new QPushButton(tr("Refresh devices"), this);
    targetRow->addWidget(refreshButton);
    root->addLayout(targetRow);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildDevicesPage(), tr("Devices"));
    m_tabs->addTab(buildAppsPage(), tr("Apps"));
    m_tabs->addTab(buildFilesPage(), tr("Files"));
    m_tabs->addTab(buildWirelessPage(), tr("Wireless"));
    m_tabs->addTab(buildDiagnosticsPage(), tr("Diagnostics"));
    m_tabs->addTab(buildShellPage(), tr("Shell & ports"));
    m_tabs->addTab(buildWorkflowsPage(), tr("Workflows"));
    root->addWidget(m_tabs, 1);

    auto *consoleHeading = new QHBoxLayout();
    consoleHeading->addWidget(new QLabel(tr("Operation output"), this));
    consoleHeading->addStretch(1);
    auto *clearConsoleButton = new QPushButton(tr("Clear"), this);
    consoleHeading->addWidget(clearConsoleButton);
    root->addLayout(consoleHeading);
    m_console = new QPlainTextEdit(this);
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(5000);
    m_console->setMinimumHeight(130);
    root->addWidget(m_console);

    connect(refreshButton, &QPushButton::clicked, this, &AdbSuiteDialog::refreshDevices);
}

QWidget *AdbSuiteDialog::buildDevicesPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *description = new QLabel(tr("Inspect attached devices and collect basic device properties."), page);
    layout->addWidget(description);
    m_deviceTable = new QTableWidget(0, 3, page);
    m_deviceTable->setHorizontalHeaderLabels(QStringList() << tr("Serial") << tr("State") << tr("Details"));
    m_deviceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_deviceTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_deviceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_deviceTable->horizontalHeader()->setStretchLastSection(true);
    m_deviceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_deviceTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(m_deviceTable, 1);
    auto *buttonRow = new QHBoxLayout();
    auto *detailsButton = new QPushButton(tr("Read device properties"), page);
    buttonRow->addWidget(detailsButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);
    connect(detailsButton, &QPushButton::clicked, this, [this] {
        runAdb(QStringList() << "shell" << "getprop", tr("Read device properties"));
    });
    return page;
}

QWidget *AdbSuiteDialog::buildAppsPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *topRow = new QHBoxLayout();
    m_includeSystemApps = new QCheckBox(tr("Include system apps"), page);
    topRow->addWidget(m_includeSystemApps);
    m_packageFilter = new QLineEdit(page);
    m_packageFilter->setPlaceholderText(tr("Filter package names"));
    topRow->addWidget(m_packageFilter, 1);
    auto *refreshButton = new QPushButton(tr("Refresh apps"), page);
    topRow->addWidget(refreshButton);
    layout->addLayout(topRow);
    m_packageList = new QListWidget(page);
    layout->addWidget(m_packageList, 1);
    auto *actions = new QHBoxLayout();
    auto *installButton = new QPushButton(tr("Install APK…"), page);
    auto *launchButton = new QPushButton(tr("Launch"), page);
    auto *inspectButton = new QPushButton(tr("Inspect"), page);
    auto *exportButton = new QPushButton(tr("Export APK…"), page);
    auto *uninstallButton = new QPushButton(tr("Uninstall…"), page);
    actions->addWidget(installButton);
    actions->addWidget(launchButton);
    actions->addWidget(inspectButton);
    actions->addWidget(exportButton);
    actions->addWidget(uninstallButton);
    layout->addLayout(actions);
    connect(refreshButton, &QPushButton::clicked, this, &AdbSuiteDialog::refreshPackages);
    connect(m_includeSystemApps, &QCheckBox::toggled, this, [this] { refreshPackages(); });
    connect(m_packageFilter, &QLineEdit::textChanged, this, &AdbSuiteDialog::filterPackages);
    connect(installButton, &QPushButton::clicked, this, &AdbSuiteDialog::installApk);
    connect(launchButton, &QPushButton::clicked, this, &AdbSuiteDialog::launchSelectedApp);
    connect(inspectButton, &QPushButton::clicked, this, &AdbSuiteDialog::inspectSelectedApp);
    connect(exportButton, &QPushButton::clicked, this, &AdbSuiteDialog::exportSelectedApk);
    connect(uninstallButton, &QPushButton::clicked, this, &AdbSuiteDialog::uninstallSelectedApp);
    connect(m_packageList, &QListWidget::itemDoubleClicked, this, [this] { launchSelectedApp(); });
    return page;
}

QWidget *AdbSuiteDialog::buildFilesPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *description = new QLabel(tr("Transfer files between this computer and the selected Android device."), page);
    layout->addWidget(description);
    auto *form = new QFormLayout();
    auto *pushRow = new QHBoxLayout();
    m_pushLocalPath = new QLineEdit(page);
    m_pushLocalPath->setPlaceholderText(tr("Choose a local file"));
    auto *browseLocalButton = new QPushButton(tr("Browse…"), page);
    pushRow->addWidget(m_pushLocalPath, 1);
    pushRow->addWidget(browseLocalButton);
    form->addRow(tr("Local file to push:"), pushRow);
    m_pushRemotePath = new QLineEdit(QStringLiteral("/sdcard/"), page);
    form->addRow(tr("Device destination:"), m_pushRemotePath);
    auto *pullRow = new QHBoxLayout();
    m_pullRemotePath = new QLineEdit(page);
    m_pullRemotePath->setPlaceholderText(tr("Device file path, e.g. /sdcard/Download/file.txt"));
    auto *browseDestinationButton = new QPushButton(tr("Save as…"), page);
    pullRow->addWidget(m_pullRemotePath, 1);
    pullRow->addWidget(browseDestinationButton);
    form->addRow(tr("Device file to pull:"), pullRow);
    m_pullLocalPath = new QLineEdit(page);
    m_pullLocalPath->setPlaceholderText(tr("Choose where to save the file on this computer"));
    form->addRow(tr("Save on computer:"), m_pullLocalPath);
    layout->addLayout(form);
    auto *actions = new QHBoxLayout();
    auto *pushButton = new QPushButton(tr("Push file"), page);
    auto *pullButton = new QPushButton(tr("Pull file"), page);
    actions->addWidget(pushButton);
    actions->addWidget(pullButton);
    actions->addStretch(1);
    layout->addLayout(actions);
    layout->addStretch(1);
    connect(browseLocalButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Select a file to push"));
        if (!path.isEmpty()) m_pushLocalPath->setText(path);
    });
    connect(browseDestinationButton, &QPushButton::clicked, this, [this] {
        const QString remote = m_pullRemotePath->text().trimmed();
        QString suggested = remote.isEmpty() ? QString() : QFileInfo(remote).fileName();
        const QString path = QFileDialog::getSaveFileName(this, tr("Save pulled file"), suggested);
        if (!path.isEmpty()) m_pullLocalPath->setText(path);
    });
    connect(pushButton, &QPushButton::clicked, this, &AdbSuiteDialog::pushFile);
    connect(pullButton, &QPushButton::clicked, this, &AdbSuiteDialog::pullFile);
    return page;
}

QWidget *AdbSuiteDialog::buildWirelessPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *description = new QLabel(
        tr("Pair using Android Wireless debugging (Android 11+). The pairing port and connection port are different; find each one on the phone's Wireless debugging screen."), page);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *form = new QFormLayout();
    m_wirelessHost = new QLineEdit(page);
    m_wirelessHost->setPlaceholderText(tr("Phone IP address"));
    form->addRow(tr("Phone address:"), m_wirelessHost);
    m_pairPort = new QSpinBox(page);
    m_pairPort->setRange(0, 65535);
    m_pairPort->setValue(0);
    form->addRow(tr("Pairing port:"), m_pairPort);
    m_pairCode = new QLineEdit(page);
    m_pairCode->setEchoMode(QLineEdit::Password);
    m_pairCode->setPlaceholderText(tr("Temporary code shown by Android"));
    form->addRow(tr("Pairing code:"), m_pairCode);
    m_connectPort = new QSpinBox(page);
    m_connectPort->setRange(0, 65535);
    m_connectPort->setValue(0);
    form->addRow(tr("Connection port:"), m_connectPort);
    layout->addLayout(form);
    auto *buttons = new QHBoxLayout();
    auto *pairButton = new QPushButton(tr("Pair device"), page);
    auto *connectButton = new QPushButton(tr("Connect paired device"), page);
    buttons->addWidget(pairButton);
    buttons->addWidget(connectButton);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    layout->addStretch(1);
    connect(pairButton, &QPushButton::clicked, this, &AdbSuiteDialog::pairWirelessDevice);
    connect(connectButton, &QPushButton::clicked, this, &AdbSuiteDialog::connectWirelessDevice);
    return page;
}

QWidget *AdbSuiteDialog::buildDiagnosticsPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *actions = new QHBoxLayout();
    auto *startButton = new QPushButton(tr("Start Logcat"), page);
    auto *stopButton = new QPushButton(tr("Stop"), page);
    auto *clearButton = new QPushButton(tr("Clear"), page);
    auto *saveLogButton = new QPushButton(tr("Save log…"), page);
    auto *bugReportButton = new QPushButton(tr("Capture bug report…"), page);
    actions->addWidget(startButton);
    actions->addWidget(stopButton);
    actions->addWidget(clearButton);
    actions->addWidget(saveLogButton);
    actions->addStretch(1);
    actions->addWidget(bugReportButton);
    layout->addLayout(actions);
    auto *filterRow = new QHBoxLayout();
    filterRow->addWidget(new QLabel(tr("Show lines containing:"), page));
    m_logcatFilter = new QLineEdit(page);
    m_logcatFilter->setPlaceholderText(tr("Optional text filter"));
    filterRow->addWidget(m_logcatFilter, 1);
    layout->addLayout(filterRow);
    m_logcatView = new QPlainTextEdit(page);
    m_logcatView->setReadOnly(true);
    m_logcatView->setMaximumBlockCount(10000);
    layout->addWidget(m_logcatView, 1);
    connect(startButton, &QPushButton::clicked, this, &AdbSuiteDialog::startLogcat);
    connect(stopButton, &QPushButton::clicked, this, &AdbSuiteDialog::stopLogcat);
    connect(clearButton, &QPushButton::clicked, m_logcatView, &QPlainTextEdit::clear);
    connect(saveLogButton, &QPushButton::clicked, this, &AdbSuiteDialog::saveLogcat);
    connect(bugReportButton, &QPushButton::clicked, this, &AdbSuiteDialog::captureBugReport);
    return page;
}

QWidget *AdbSuiteDialog::buildShellPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *shellGroup = new QGroupBox(tr("ADB shell"), page);
    auto *shellLayout = new QVBoxLayout(shellGroup);
    shellLayout->addWidget(new QLabel(tr("Run a shell command on the selected device. Commands are sent through ADB directly."), shellGroup));
    auto *shellRow = new QHBoxLayout();
    m_shellCommand = new QLineEdit(shellGroup);
    m_shellCommand->setPlaceholderText(tr("Example: dumpsys battery"));
    auto *runShellButton = new QPushButton(tr("Run command"), shellGroup);
    shellRow->addWidget(m_shellCommand, 1);
    shellRow->addWidget(runShellButton);
    shellLayout->addLayout(shellRow);
    layout->addWidget(shellGroup);

    auto *portsGroup = new QGroupBox(tr("Port forwarding"), page);
    auto *portsLayout = new QFormLayout(portsGroup);
    m_portDirection = new QComboBox(portsGroup);
    m_portDirection->addItem(tr("Forward: host → device"), QStringLiteral("forward"));
    m_portDirection->addItem(tr("Reverse: device → host"), QStringLiteral("reverse"));
    portsLayout->addRow(tr("Direction:"), m_portDirection);
    m_hostPort = new QSpinBox(portsGroup);
    m_hostPort->setRange(1, 65535);
    m_hostPort->setValue(8080);
    portsLayout->addRow(tr("Host port:"), m_hostPort);
    m_devicePort = new QSpinBox(portsGroup);
    m_devicePort->setRange(1, 65535);
    m_devicePort->setValue(8080);
    portsLayout->addRow(tr("Device port:"), m_devicePort);
    auto *portButtons = new QHBoxLayout();
    auto *createRuleButton = new QPushButton(tr("Create rule"), portsGroup);
    auto *listRulesButton = new QPushButton(tr("List rules"), portsGroup);
    auto *removeRuleButton = new QPushButton(tr("Remove rule"), portsGroup);
    portButtons->addWidget(createRuleButton);
    portButtons->addWidget(listRulesButton);
    portButtons->addWidget(removeRuleButton);
    portsLayout->addRow(portButtons);
    layout->addWidget(portsGroup);
    layout->addStretch(1);
    connect(runShellButton, &QPushButton::clicked, this, &AdbSuiteDialog::runShellCommand);
    connect(m_shellCommand, &QLineEdit::returnPressed, this, &AdbSuiteDialog::runShellCommand);
    connect(createRuleButton, &QPushButton::clicked, this, &AdbSuiteDialog::createPortRule);
    connect(listRulesButton, &QPushButton::clicked, this, &AdbSuiteDialog::listPortRules);
    connect(removeRuleButton, &QPushButton::clicked, this, &AdbSuiteDialog::removePortRule);
    return page;
}

QWidget *AdbSuiteDialog::buildWorkflowsPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *description = new QLabel(
        tr("Create reusable device workflows. Enter one Android shell command per line; lines starting with # are comments. Workflows are stored as JSON."), page);
    description->setWordWrap(true);
    layout->addWidget(description);
    m_workflowEditor = new QPlainTextEdit(page);
    m_workflowEditor->setPlaceholderText(tr("# Example workflow\ndumpsys battery\ngetprop ro.build.version.release"));
    layout->addWidget(m_workflowEditor, 1);
    m_runWorkflowOnAll = new QCheckBox(tr("Run on all authorized connected devices"), page);
    layout->addWidget(m_runWorkflowOnAll);
    auto *buttons = new QHBoxLayout();
    auto *loadButton = new QPushButton(tr("Load JSON…"), page);
    auto *saveButton = new QPushButton(tr("Save JSON…"), page);
    auto *runButton = new QPushButton(tr("Run workflow"), page);
    buttons->addWidget(loadButton);
    buttons->addWidget(saveButton);
    buttons->addStretch(1);
    buttons->addWidget(runButton);
    layout->addLayout(buttons);
    connect(loadButton, &QPushButton::clicked, this, &AdbSuiteDialog::loadWorkflow);
    connect(saveButton, &QPushButton::clicked, this, &AdbSuiteDialog::saveWorkflow);
    connect(runButton, &QPushButton::clicked, this, &AdbSuiteDialog::runWorkflow);
    return page;
}

QString AdbSuiteDialog::selectedDevice() const
{
    return m_deviceBox->currentData().toString().trimmed();
}

void AdbSuiteDialog::runAdb(const QStringList &arguments, const QString &description,
                            const Completion &completion, bool useTarget,
                            const QString &targetOverride)
{
    if (m_commandProcess.state() != QProcess::NotRunning) {
        appendOutput(tr("Wait for the current ADB command to finish."));
        return;
    }

    QStringList adbArguments;
    if (useTarget) {
        const QString serial = targetOverride.isEmpty() ? selectedDevice() : targetOverride;
        if (serial.isEmpty()) {
            appendOutput(tr("Select an authorized device first."));
            return;
        }
        adbArguments << "-s" << serial;
    }
    adbArguments << arguments;
    m_operationDescription = description;
    if (useTarget) {
        const QString serial = targetOverride.isEmpty() ? selectedDevice() : targetOverride;
        m_operationDescription += tr(" [device: %1]").arg(serial);
    }
    m_commandOutput.clear();
    m_errorOutput.clear();
    m_completion = completion;
    m_commandCompleted = false;
    appendOutput(tr("Running: %1").arg(description));
    m_commandProcess.start(m_adbPath, adbArguments);
}

void AdbSuiteDialog::appendOutput(const QString &text)
{
    if (!m_console || text.trimmed().isEmpty()) return;
    const QStringList lines = text.split(QRegularExpression("\\r?\\n"), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        m_console->appendPlainText(QStringLiteral("[%1] %2")
                                   .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), line));
    }
}

void AdbSuiteDialog::refreshDevices()
{
    runAdb(QStringList() << "devices" << "-l", tr("Refresh devices"),
           [this](int exitCode, const QString &output, const QString &) {
        if (exitCode != 0) return;
        const QString previous = selectedDevice();
        m_onlineDevices.clear();
        m_deviceTable->setRowCount(0);
        const QStringList lines = output.split(QRegularExpression("\\r?\\n"), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QString normalized = line.trimmed();
            if (normalized.isEmpty() || normalized.startsWith("List of devices")) continue;
            const QStringList fields = normalized.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (fields.size() < 2) continue;
            const int row = m_deviceTable->rowCount();
            m_deviceTable->insertRow(row);
            m_deviceTable->setItem(row, 0, new QTableWidgetItem(fields.at(0)));
            m_deviceTable->setItem(row, 1, new QTableWidgetItem(fields.at(1)));
            m_deviceTable->setItem(row, 2, new QTableWidgetItem(fields.mid(2).join(" ")));
            if (fields.at(1) == "device") m_onlineDevices.append(fields.at(0));
        }
        m_deviceBox->clear();
        for (const QString &serial : m_onlineDevices) m_deviceBox->addItem(serial, serial);
        const int index = m_deviceBox->findData(previous);
        if (index >= 0) m_deviceBox->setCurrentIndex(index);
        if (m_deviceBox->count() == 0) appendOutput(tr("No authorized devices are connected."));
    }, false);
}

void AdbSuiteDialog::refreshPackages()
{
    QStringList arguments;
    arguments << "shell" << "pm" << "list" << "packages";
    if (!m_includeSystemApps->isChecked()) arguments << "-3";
    runAdb(arguments, tr("List installed apps"), [this](int exitCode, const QString &output, const QString &) {
        if (exitCode != 0) return;
        m_packageList->clear();
        const QStringList lines = output.split(QRegularExpression("\\r?\\n"), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            QString package = line.trimmed();
            if (package.startsWith("package:")) package.remove(0, 8);
            if (!package.isEmpty()) m_packageList->addItem(package);
        }
        filterPackages(m_packageFilter->text());
        appendOutput(tr("Found %1 apps.").arg(m_packageList->count()));
    });
}

void AdbSuiteDialog::inspectSelectedApp()
{
    if (!m_packageList->currentItem()) {
        appendOutput(tr("Select an app package first."));
        return;
    }
    const QString package = m_packageList->currentItem()->text();
    runAdb(QStringList() << "shell" << "dumpsys" << "package" << package,
           tr("Inspect package, activities, and permissions for %1").arg(package));
}

void AdbSuiteDialog::filterPackages(const QString &filter)
{
    for (int i = 0; i < m_packageList->count(); ++i) {
        QListWidgetItem *item = m_packageList->item(i);
        item->setHidden(!item->text().contains(filter, Qt::CaseInsensitive));
    }
}

void AdbSuiteDialog::installApk()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Select APK to install"), QString(),
                                                       tr("Android packages (*.apk);;All files (*.*)"));
    if (path.isEmpty()) return;
    runAdb(QStringList() << "install" << "-r" << path,
           tr("Install %1").arg(QFileInfo(path).fileName()),
           [this](int exitCode, const QString &, const QString &) {
        if (exitCode == 0) refreshPackages();
    });
}

void AdbSuiteDialog::launchSelectedApp()
{
    if (!m_packageList->currentItem()) {
        appendOutput(tr("Select an app package first."));
        return;
    }
    const QString package = m_packageList->currentItem()->text();
    runAdb(QStringList() << "shell" << "monkey" << "-p" << package << "1",
           tr("Launch %1").arg(package));
}

void AdbSuiteDialog::uninstallSelectedApp()
{
    if (!m_packageList->currentItem()) {
        appendOutput(tr("Select an app package first."));
        return;
    }
    const QString package = m_packageList->currentItem()->text();
    const QString message = tr("Uninstall %1 from device %2?")
                                .arg(package, selectedDevice());
    if (QMessageBox::question(this, tr("Confirm uninstall"), message,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    runAdb(QStringList() << "uninstall" << package, tr("Uninstall %1").arg(package),
           [this](int exitCode, const QString &, const QString &) {
        if (exitCode == 0) refreshPackages();
    });
}

void AdbSuiteDialog::exportSelectedApk()
{
    if (!m_packageList->currentItem()) {
        appendOutput(tr("Select an app package first."));
        return;
    }
    const QString package = m_packageList->currentItem()->text();
    runAdb(QStringList() << "shell" << "pm" << "path" << package,
           tr("Find APK path for %1").arg(package),
           [this, package](int exitCode, const QString &output, const QString &) {
        if (exitCode != 0) return;
        QString remotePath;
        const QStringList lines = output.split(QRegularExpression("\\r?\\n"), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QString value = line.trimmed();
            if (value.startsWith("package:")) {
                remotePath = value.mid(8);
                break;
            }
        }
        if (remotePath.isEmpty()) {
            appendOutput(tr("The device did not return an APK path for this app."));
            return;
        }
        const QString path = QFileDialog::getSaveFileName(this, tr("Export base APK"), package + ".apk",
                                                           tr("Android packages (*.apk)"));
        if (!path.isEmpty()) runAdb(QStringList() << "pull" << remotePath << path,
                                    tr("Export %1 APK").arg(package));
    });
}

void AdbSuiteDialog::pushFile()
{
    const QString localPath = m_pushLocalPath->text().trimmed();
    const QString remotePath = m_pushRemotePath->text().trimmed();
    if (localPath.isEmpty() || !QFileInfo::exists(localPath) || remotePath.isEmpty()) {
        appendOutput(tr("Choose an existing local file and enter a device destination."));
        return;
    }
    runAdb(QStringList() << "push" << localPath << remotePath,
           tr("Push %1").arg(QFileInfo(localPath).fileName()));
}

void AdbSuiteDialog::pullFile()
{
    const QString remotePath = m_pullRemotePath->text().trimmed();
    const QString localPath = m_pullLocalPath->text().trimmed();
    if (remotePath.isEmpty() || localPath.isEmpty()) {
        appendOutput(tr("Enter a device file path and choose Save as first."));
        return;
    }
    runAdb(QStringList() << "pull" << remotePath << localPath,
           tr("Pull %1").arg(QFileInfo(remotePath).fileName()));
}

void AdbSuiteDialog::pairWirelessDevice()
{
    QString host = m_wirelessHost->text().trimmed();
    if (host.isEmpty() || m_pairCode->text().trimmed().isEmpty() || m_pairPort->value() == 0) {
        appendOutput(tr("Enter the phone address, pairing port, and temporary pairing code."));
        return;
    }
    if (host.contains(':') && !host.startsWith('[')) host = QStringLiteral("[") + host + QStringLiteral("]");
    const QString endpoint = host + QStringLiteral(":") + QString::number(m_pairPort->value());
    runAdb(QStringList() << "pair" << endpoint << m_pairCode->text().trimmed(),
           tr("Pair wireless device"), Completion(), false);
    m_pairCode->clear();
}

void AdbSuiteDialog::connectWirelessDevice()
{
    QString host = m_wirelessHost->text().trimmed();
    if (host.isEmpty() || m_connectPort->value() == 0) {
        appendOutput(tr("Enter the phone address and connection port."));
        return;
    }
    if (host.contains(':') && !host.startsWith('[')) host = QStringLiteral("[") + host + QStringLiteral("]");
    const QString endpoint = host + QStringLiteral(":") + QString::number(m_connectPort->value());
    runAdb(QStringList() << "connect" << endpoint, tr("Connect wireless device"),
           [this](int exitCode, const QString &, const QString &) {
        if (exitCode == 0) refreshDevices();
    }, false);
}

void AdbSuiteDialog::startLogcat()
{
    if (m_logcatProcess.state() != QProcess::NotRunning) return;
    const QString serial = selectedDevice();
    if (serial.isEmpty()) {
        appendOutput(tr("Select an authorized device first."));
        return;
    }
    m_logcatPending.clear();
    m_deviceBox->setEnabled(false);
    m_logcatView->appendPlainText(tr("Starting Logcat for %1…").arg(serial));
    m_logcatProcess.start(m_adbPath, QStringList() << "-s" << serial << "logcat" << "-v" << "time");
}

void AdbSuiteDialog::stopLogcat()
{
    if (m_logcatProcess.state() == QProcess::NotRunning) return;
    m_logcatProcess.terminate();
    if (!m_logcatProcess.waitForFinished(1000)) m_logcatProcess.kill();
    m_deviceBox->setEnabled(true);
    appendOutput(tr("Logcat stopped."));
}

void AdbSuiteDialog::saveLogcat()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Save current Logcat output"),
                                                       QStringLiteral("QtScrcpy-logcat.txt"),
                                                       tr("Text files (*.txt);;All files (*.*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    const QByteArray bytes = m_logcatView->toPlainText().toUtf8();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()) {
        QMessageBox::warning(this, tr("Save Logcat"), tr("Could not write the Logcat file."));
        return;
    }
    appendOutput(tr("Saved Logcat output to %1").arg(path));
}

void AdbSuiteDialog::captureBugReport()
{
    const QString defaultName = QStringLiteral("QtScrcpy-bugreport-%1.zip")
                                    .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Save device bug report"), defaultName,
                                                       tr("Bug report archive (*.zip);;All files (*.*)"));
    if (path.isEmpty()) return;
    runAdb(QStringList() << "bugreport" << path, tr("Capture bug report"));
}

void AdbSuiteDialog::runShellCommand()
{
    const QString command = m_shellCommand->text().trimmed();
    if (command.isEmpty()) return;
    runAdb(QStringList() << "shell" << command, tr("Shell: %1").arg(command));
}

void AdbSuiteDialog::createPortRule()
{
    const QString direction = m_portDirection->currentData().toString();
    QStringList arguments;
    if (direction == "forward") {
        arguments << "forward" << QString("tcp:%1").arg(m_hostPort->value())
                  << QString("tcp:%1").arg(m_devicePort->value());
    } else {
        arguments << "reverse" << QString("tcp:%1").arg(m_devicePort->value())
                  << QString("tcp:%1").arg(m_hostPort->value());
    }
    runAdb(arguments, tr("Create %1 port rule").arg(direction));
}

void AdbSuiteDialog::listPortRules()
{
    runAdb(QStringList() << m_portDirection->currentData().toString() << "--list",
           tr("List %1 port rules").arg(m_portDirection->currentData().toString()));
}

void AdbSuiteDialog::removePortRule()
{
    const QString direction = m_portDirection->currentData().toString();
    QStringList arguments;
    if (direction == "forward") {
        arguments << "forward" << "--remove" << QString("tcp:%1").arg(m_hostPort->value());
    } else {
        arguments << "reverse" << "--remove" << QString("tcp:%1").arg(m_devicePort->value());
    }
    if (QMessageBox::question(this, tr("Remove port rule"),
                              tr("Remove the selected %1 port mapping from %2?").arg(direction, selectedDevice()),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    runAdb(arguments, tr("Remove %1 port rule").arg(direction));
}

QStringList AdbSuiteDialog::workflowCommands() const
{
    QStringList commands;
    const QStringList lines = m_workflowEditor->toPlainText().split(QRegularExpression("\\r?\\n"));
    for (const QString &line : lines) {
        const QString command = line.trimmed();
        if (!command.isEmpty() && !command.startsWith('#')) commands.append(command);
    }
    return commands;
}

void AdbSuiteDialog::saveWorkflow()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Save ADB workflow"), QString(),
                                                       tr("QtScrcpy workflows (*.json)"));
    if (path.isEmpty()) return;
    QJsonArray commands;
    const QStringList lines = workflowCommands();
    for (const QString &line : lines) commands.append(line);
    QJsonObject root;
    root.insert("version", 1);
    root.insert("commands", commands);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0
            || !file.commit()) {
        QMessageBox::warning(this, tr("Save workflow"), tr("Could not write the workflow JSON file."));
        return;
    }
    appendOutput(tr("Workflow saved to %1").arg(path));
}

void AdbSuiteDialog::loadWorkflow()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Load ADB workflow"), QString(),
                                                       tr("QtScrcpy workflows (*.json);;All files (*.*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Load workflow"), tr("Could not read the workflow file."));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || !document.object().value("commands").isArray()) {
        QMessageBox::warning(this, tr("Load workflow"), tr("This is not a valid QtScrcpy workflow JSON file."));
        return;
    }
    QStringList commands;
    const QJsonArray array = document.object().value("commands").toArray();
    for (const QJsonValue &value : array) {
        if (!value.isString() || value.toString().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Load workflow"), tr("Every workflow command must be a non-empty string."));
            return;
        }
        commands.append(value.toString());
    }
    m_workflowEditor->setPlainText(commands.join("\n"));
    appendOutput(tr("Loaded %1 workflow steps from %2").arg(commands.size()).arg(path));
}

void AdbSuiteDialog::runWorkflow()
{
    const QStringList commands = workflowCommands();
    if (commands.isEmpty()) {
        appendOutput(tr("Add at least one shell command to the workflow."));
        return;
    }
    QStringList targets;
    if (m_runWorkflowOnAll->isChecked()) {
        targets = m_onlineDevices;
        if (targets.isEmpty()) {
            appendOutput(tr("There are no authorized connected devices."));
            return;
        }
        const QString message = tr("Run %1 commands on all %2 connected device(s)?\n\n%3")
                                    .arg(commands.size()).arg(targets.size())
                                    .arg(targets.join("\n") + tr("\n\nCommands:\n") + commands.join("\n"));
        if (QMessageBox::question(this, tr("Run workflow on all devices"), message,
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    } else {
        if (selectedDevice().isEmpty()) {
            appendOutput(tr("Select an authorized device first."));
            return;
        }
        targets << selectedDevice();
    }
    appendOutput(tr("Starting workflow: %1 step(s) on %2 device(s).")
                 .arg(commands.size()).arg(targets.size()));
    runWorkflowStep(targets, commands, 0, 0);
}

void AdbSuiteDialog::runWorkflowStep(const QStringList &targets, const QStringList &commands,
                                     int targetIndex, int commandIndex)
{
    if (targetIndex >= targets.size()) {
        appendOutput(tr("Workflow completed."));
        return;
    }
    if (commandIndex >= commands.size()) {
        runWorkflowStep(targets, commands, targetIndex + 1, 0);
        return;
    }
    const QString serial = targets.at(targetIndex);
    const QString command = commands.at(commandIndex);
    appendOutput(tr("Workflow %1/%2 on %3: %4")
                 .arg(commandIndex + 1).arg(commands.size()).arg(serial, command));
    runAdb(QStringList() << "shell" << command,
           tr("Workflow command on %1").arg(serial),
           [this, targets, commands, targetIndex, commandIndex](int exitCode, const QString &, const QString &) {
        if (exitCode != 0) {
            appendOutput(tr("Workflow stopped after a command failed."));
            return;
        }
        runWorkflowStep(targets, commands, targetIndex, commandIndex + 1);
    }, true, serial);
}

void AdbSuiteDialog::closeEvent(QCloseEvent *event)
{
    stopLogcat();
    QDialog::closeEvent(event);
}
