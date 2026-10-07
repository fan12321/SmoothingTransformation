#include "SmoothingTransformation.h"

#include <PointData/PointData.h>

#include <actions/IntegralAction.h>
#include <actions/WidgetActionViewWidget.h>

#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>
#include <vector>

Q_PLUGIN_METADATA(IID "studio.manivault.SmoothingTransformation")

using namespace mv;

using Kernel = SmoothingTransformation::Kernel;
using Output = SmoothingTransformation::Output;

const QMap<Kernel, QString> SmoothingTransformation::kernels = QMap<Kernel, QString>({
    { Kernel::MovingAverage, "Simple moving average" },
    { Kernel::SavitzkyGolay, "Savitzky-Golay" }
});

const QMap<Output, QString> SmoothingTransformation::outputs = QMap<Output, QString>({
    { Output::InPlace, "Replace existing dataset" },
    { Output::Derived, "Add derived dataset" }
});

namespace {

constexpr int maxPolynomialOrder = 10;

bool solveLinearSystem(std::vector<std::vector<double>>& matrix, std::vector<double>& rhs)
{
    const int size = static_cast<int>(rhs.size());

    for (int pivot = 0; pivot < size; ++pivot) {
        int bestRow = pivot;
        double bestValue = std::abs(matrix[pivot][pivot]);

        for (int row = pivot + 1; row < size; ++row) {
            const double value = std::abs(matrix[row][pivot]);
            if (value > bestValue) {
                bestValue = value;
                bestRow = row;
            }
        }

        if (bestValue < 1e-12)
            return false;

        if (bestRow != pivot) {
            std::swap(matrix[pivot], matrix[bestRow]);
            std::swap(rhs[pivot], rhs[bestRow]);
        }

        const double inversePivot = 1.0 / matrix[pivot][pivot];

        for (int col = pivot; col < size; ++col)
            matrix[pivot][col] *= inversePivot;

        rhs[pivot] *= inversePivot;

        for (int row = 0; row < size; ++row) {
            if (row == pivot)
                continue;

            const double factor = matrix[row][pivot];
            if (std::abs(factor) < 1e-12)
                continue;

            for (int col = pivot; col < size; ++col)
                matrix[row][col] -= factor * matrix[pivot][col];

            rhs[row] -= factor * rhs[pivot];
        }
    }

    return true;
}

std::vector<float> applyKernel(const std::vector<float>& input, const std::vector<float>& kernel)
{
    std::vector<float> output(input.size(), 0.0f);

    if (kernel.empty() || input.empty())
        return output;

    const int radius = static_cast<int>(kernel.size()) / 2;

    for (std::size_t i = 0; i < input.size(); ++i) {
        double sum = 0.0;

        for (std::size_t k = 0; k < kernel.size(); ++k) {
            const int sourceIndex = static_cast<int>(i) + static_cast<int>(k) - radius;
            const int clampedIndex = std::clamp(sourceIndex, 0, static_cast<int>(input.size()) - 1);
            sum += static_cast<double>(kernel[k]) * input[clampedIndex];
        }

        output[i] = static_cast<float>(sum);
    }

    return output;
}

std::vector<float> movingAverageKernel(int windowSize)
{
    const auto size = std::max(1, windowSize);
    return std::vector<float>(static_cast<std::size_t>(size), 1.0f / static_cast<float>(size));
}

std::vector<float> savitzkyGolayKernel(int windowSize, int polynomialOrder)
{
    const int effectiveWindow = std::max(1, windowSize);
    const int effectiveOrder = std::clamp(polynomialOrder, 0, std::max(0, effectiveWindow - 1));
    const int radius = effectiveWindow / 2;

    std::vector<double> positions(static_cast<std::size_t>(effectiveWindow));
    for (int i = 0; i < effectiveWindow; ++i)
        positions[i] = static_cast<double>(i - radius);

    const int degree = effectiveOrder;
    std::vector<std::vector<double>> system(static_cast<std::size_t>(degree + 1), std::vector<double>(static_cast<std::size_t>(degree + 1), 0.0));
    std::vector<double> rhs(static_cast<std::size_t>(degree + 1), 0.0);

    rhs[0] = 1.0;

    for (int row = 0; row <= degree; ++row) {
        for (int col = 0; col <= degree; ++col) {
            double sum = 0.0;

            for (int i = 0; i < effectiveWindow; ++i)
                sum += std::pow(positions[i], static_cast<double>(row + col));

            system[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] = sum;
        }
    }

    if (!solveLinearSystem(system, rhs)) {
        qDebug() << "Unable to solve Savitzky-Golay system";
        return std::vector<float>(static_cast<std::size_t>(effectiveWindow), 1.0f / static_cast<float>(effectiveWindow));
    }

    std::vector<float> weights(static_cast<std::size_t>(effectiveWindow), 0.0f);
    for (int i = 0; i < effectiveWindow; ++i) {
        double value = 0.0;
        for (int degreeIndex = 0; degreeIndex <= degree; ++degreeIndex)
            value += rhs[degreeIndex] * std::pow(positions[i], static_cast<double>(degreeIndex));

        weights[i] = static_cast<float>(value);
    }

    return weights;
}

std::vector<float> smoothSpectrum(const std::vector<float>& spectrum, Kernel kernel, int windowSize, int polynomialOrder)
{
    if (spectrum.empty())
        return {};

    switch (kernel) {
        case Kernel::MovingAverage:
            return applyKernel(spectrum, movingAverageKernel(windowSize));
        case Kernel::SavitzkyGolay:
            return applyKernel(spectrum, savitzkyGolayKernel(windowSize, polynomialOrder));
    }

    return spectrum;
}

} // namespace

SmoothingTransformation::SmoothingTransformation(const PluginFactory* factory) :
    TransformationPlugin(factory),
    _kernel(Kernel::SavitzkyGolay),
    _output(Output::Derived),
    _maWindowSize(5),
    _sgWindowSize(7),
    _sgPolynomialOrder(2)
{
}

void SmoothingTransformation::transform()
{
    auto points = getInputDataset<Points>();

    if (!points.isValid())
        return;

    const auto numDimensions = static_cast<int>(points->getNumDimensions());
    const auto numPoints = points->getNumPoints();

    if (numDimensions <= 0) {
        qWarning() << "Smoothing Transformation: input dataset has no dimensions";
        return;
    }

    auto kernelDescription = getKernelName(_kernel);
    if (_kernel == Kernel::MovingAverage)
        kernelDescription += QString(" w%1").arg(_maWindowSize);
    if (_kernel == Kernel::SavitzkyGolay)
        kernelDescription += QString(" w%1 p%2").arg(_sgWindowSize).arg(_sgPolynomialOrder);

    auto& task = points->getTask();

    task.setName("Smoothing Transformation");
    task.setRunning();
    task.setProgressDescription(QString("Smoothing, %1 (%2)").arg(kernelDescription, getOutputName(_output)));

    std::vector<float> input(static_cast<std::size_t>(numPoints) * static_cast<std::size_t>(numDimensions));
    points->visitData([&](auto pointData) {
        std::size_t i = 0;
        for (const auto point : pointData) {
            for (int d = 0; d < numDimensions; ++d)
                input[i++] = static_cast<float>(point[d]);
        }
    });

    std::vector<float> smoothedValues(input.size(), 0.0f);

    const auto kernelWindow = _kernel == Kernel::MovingAverage ? _maWindowSize : _sgWindowSize;
    const auto polynomialOrder = _kernel == Kernel::MovingAverage ? 1 : _sgPolynomialOrder;

    for (std::size_t pointIndex = 0; pointIndex < static_cast<std::size_t>(numPoints); ++pointIndex) {
        std::vector<float> spectrum(static_cast<std::size_t>(numDimensions));

        for (int d = 0; d < numDimensions; ++d)
            spectrum[d] = input[pointIndex * static_cast<std::size_t>(numDimensions) + static_cast<std::size_t>(d)];

        const auto smoothedSpectrum = smoothSpectrum(spectrum, _kernel, kernelWindow, polynomialOrder);

        for (int d = 0; d < numDimensions; ++d)
            smoothedValues[pointIndex * static_cast<std::size_t>(numDimensions) + static_cast<std::size_t>(d)] = smoothedSpectrum[static_cast<std::size_t>(d)];
    }

    input.clear();
    input.shrink_to_fit();

    const auto smoothingDimensionNames = [&points, numDimensions]() -> std::vector<QString> {
        auto dimensionNames = points->getDimensionNames();

        if (static_cast<int>(dimensionNames.size()) != numDimensions) {
            dimensionNames.resize(numDimensions);

            for (int d = 0; d < numDimensions; ++d)
                dimensionNames[d] = QString::number(d);
        }

        for (auto& name : dimensionNames)
            name = QString("%1").arg(name);

        return dimensionNames;
    };


    switch (_output) {
        case Output::InPlace:
        {
            points->setLocked(true);

            points->visitData([&smoothedValues, numDimensions](auto pointData) {
                std::size_t i = 0;
                for (auto point : pointData) {
                    for (int d = 0; d < numDimensions; ++d) {
                        using ValueType = std::remove_reference_t<decltype(point[d])>;
                        point[d] = static_cast<ValueType>(smoothedValues[i++]);
                    }
                }
            });

            points->setLocked(false);

            if (points->isFull())
                points->setDimensionNames(smoothingDimensionNames());

            events().notifyDatasetDataChanged(points);
            break;
        }

        case Output::Derived:
        {
            auto derived = mv::data().createDerivedDataset<Points>(
                points->getGuiName() + QString(" (smoothed, %1)").arg(kernelDescription), points);

            derived->setData(std::move(smoothedValues), numDimensions);
            derived->setDimensionNames(smoothingDimensionNames());

            events().notifyDatasetDataChanged(derived);
            break;
        }
    }

    task.setFinished();
}

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------

SmoothingTransformationFactory::Settings::Settings(QObject* parent, const QString& title, const Output& defaultOutput) :
    mv::gui::GroupAction(parent, title),
    _kernelAction(this, "Kernel", SmoothingTransformation::kernels.values(), SmoothingTransformation::getKernelName(Kernel::SavitzkyGolay)),
    _outputAction(this, "Output", { SmoothingTransformation::getOutputName(Output::InPlace), SmoothingTransformation::getOutputName(Output::Derived) }, SmoothingTransformation::getOutputName(defaultOutput)),
    _maWindowSizeAction(this, "Moving Average Window size", 3, 51, 5),
    _sgWindowSizeAction(this, "Savitzky-Golay Window size", 3, 101, 7),
    _sgPolynomialOrderAction(this, "Polynomial order", 1, 10, 2),
    _sgWindowSizeLast(_sgWindowSizeAction.getValue()), 
    _maWindowSizeLast(_maWindowSizeAction.getValue())
{
    _kernelAction.setToolTip("Smoothing kernel applied to every spectrum");
    _outputAction.setToolTip("Whether the smoothed values overwrite the input dataset or are written to a derived one");
    _maWindowSizeAction.setToolTip("Window size for the simple moving average filter (odd number)");
    _sgWindowSizeAction.setToolTip("Sliding window size in samples (odd number)");
    _sgPolynomialOrderAction.setToolTip("Polynomial order used by the Savitzky-Golay filter");

    connect(&_maWindowSizeAction, &mv::gui::IntegralAction::valueChanged, this, [this]() -> void {
        constrainMovingAverageWindowSize();
    });

    connect(&_sgWindowSizeAction, &mv::gui::IntegralAction::valueChanged, this, [this]() -> void {
        constrainSavitzkyGolayParameters();
    });

    connect(&_kernelAction, &mv::gui::OptionAction::currentTextChanged, this, [this]() -> void {
        constrainParametersToKernel();
    });

    constrainSavitzkyGolayParameters();
    constrainMovingAverageWindowSize();
    constrainParametersToKernel();

    setToolTip("Smoothing settings");
    setLabelSizingType(mv::gui::GroupAction::LabelSizingType::Auto);

    addAction(&_kernelAction);
    addAction(&_outputAction);
    addAction(&_maWindowSizeAction);
    addAction(&_sgWindowSizeAction);
    addAction(&_sgPolynomialOrderAction);
}

Kernel SmoothingTransformationFactory::Settings::getKernel() const
{
    return SmoothingTransformation::kernels.key(_kernelAction.getCurrentText(), Kernel::SavitzkyGolay);
}

SmoothingTransformation::Output SmoothingTransformationFactory::Settings::getOutput() const
{
    return SmoothingTransformation::outputs.key(_outputAction.getCurrentText(), Output::Derived);
}

int SmoothingTransformationFactory::Settings::getMovingAverageWindowSize() const
{
    return _maWindowSizeAction.getValue();
}

void SmoothingTransformationFactory::Settings::getSavitzkyGolayParameters(int& windowSize, int& polynomialOrder) const
{
    windowSize = _sgWindowSizeAction.getValue();
    polynomialOrder = _sgPolynomialOrderAction.getValue();
}

void SmoothingTransformationFactory::Settings::constrainParametersToKernel()
{
    const auto kernel = getKernel();

    _maWindowSizeAction.setForceDisabled(kernel != Kernel::MovingAverage);
    _sgWindowSizeAction.setForceDisabled(kernel != Kernel::SavitzkyGolay);
    _sgPolynomialOrderAction.setForceDisabled(kernel != Kernel::SavitzkyGolay);
}

void SmoothingTransformationFactory::Settings::constrainMovingAverageWindowSize() {
    const auto windowSize = _maWindowSizeAction.getValue();

    if (windowSize % 2 == 0) {
        _maWindowSizeAction.setValue(windowSize > _maWindowSizeLast ? windowSize + 1 : windowSize - 1);
        return;
    }

    _maWindowSizeLast = windowSize;
}

void SmoothingTransformationFactory::Settings::constrainSavitzkyGolayParameters()
{
    const auto windowSize = _sgWindowSizeAction.getValue();

    if (windowSize % 2 == 0) {
        _sgWindowSizeAction.setValue(windowSize > _sgWindowSizeLast ? windowSize + 1 : windowSize - 1);
        return;
    }

    _sgWindowSizeLast = windowSize;

    const auto highestOrder = std::min(maxPolynomialOrder, windowSize - 1);
    _sgPolynomialOrderAction.setMaximum(highestOrder);
    _sgPolynomialOrderAction.setValue(std::min(_sgPolynomialOrderAction.getValue(), highestOrder));
}

QWidget* SmoothingTransformationFactory::Settings::getWidget(QWidget* parent, const std::int32_t& widgetFlags)
{
    auto settingsWidget = createSettingsWidget(parent);

    if (!(widgetFlags & mv::gui::WidgetActionViewWidget::PopupLayout))
        return settingsWidget;

    auto popupWidget = new QWidget(parent);
    auto popupLayout = new QVBoxLayout(popupWidget);
    auto groupBox = new QGroupBox(text(), popupWidget);
    auto groupLayout = new QVBoxLayout(groupBox);

    popupLayout->setContentsMargins(4, 4, 4, 4);
    groupLayout->addWidget(settingsWidget);
    popupLayout->addWidget(groupBox);

    return popupWidget;
}

QWidget* SmoothingTransformationFactory::Settings::createSettingsWidget(QWidget* parent)
{
    auto widget = new QWidget(parent);
    auto layout = new QVBoxLayout(widget);

    layout->setContentsMargins(0, 0, 0, 0);

    const auto addGroupBox = [widget, layout](const QString& title, const QVector<mv::gui::WidgetAction*>& actions, bool showLabels) -> void {
        auto groupBox = new QGroupBox(title, widget);
        auto gridLayout = new QGridLayout(groupBox);

        for (auto action : actions) {
            const auto row = gridLayout->rowCount();

            if (showLabels)
                gridLayout->addWidget(action->createLabelWidget(groupBox), row, 0);

            gridLayout->addWidget(action->createWidget(groupBox), row, showLabels ? 1 : 0, 1, showLabels ? 1 : 2);
        }

        gridLayout->setColumnStretch(1, 1);
        layout->addWidget(groupBox);
    };

    addGroupBox("Parameters", { &_kernelAction, &_maWindowSizeAction, &_sgWindowSizeAction, &_sgPolynomialOrderAction }, true);
    addGroupBox("Output", { &_outputAction }, false);

    return widget;
}

bool SmoothingTransformationFactory::Settings::edit()
{
    const auto kernel = _kernelAction.getCurrentText();
    const auto output = _outputAction.getCurrentText();
    const auto movingAverageWindow = _maWindowSizeAction.getValue();
    const auto windowSize = _sgWindowSizeAction.getValue();
    const auto polynomialOrder = _sgPolynomialOrderAction.getValue();

    QDialog dialog;
    dialog.setWindowTitle("Smoothing Transformation");

    auto layout = new QVBoxLayout(&dialog);
    layout->addWidget(createSettingsWidget(&dialog));

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() == QDialog::Accepted)
        return true;

    _kernelAction.setCurrentText(kernel);
    _outputAction.setCurrentText(output);
    _maWindowSizeAction.setValue(movingAverageWindow);
    _sgWindowSizeAction.setValue(windowSize);
    _sgPolynomialOrderAction.setValue(polynomialOrder);

    return false;
}

SmoothingTransformationFactory::SmoothingTransformationFactory() :
    _hierarchySettings(this, "Smoothing", Output::Derived),
    _hostSettings(this, "Smoothing", Output::InPlace)
{
    getPluginMetadata().setDescription("Smooth spectral response functions");
    getPluginMetadata().setSummary("Smooths each point's spectrum using a moving average or Savitzky-Golay filter.");
    getPluginMetadata().setCopyrightHolder({ "REPLACE ME" });
    getPluginMetadata().setAuthors({ { "REPLACE ME", { "Developer" }, { "REPLACE ME org" } } });
    getPluginMetadata().setLicenseText("REPLACE ME (e.g. LGPL v3.0)");
}

SmoothingTransformation* SmoothingTransformationFactory::produce()
{
    return new SmoothingTransformation(this);
}

SmoothingTransformationFactory::Settings& SmoothingTransformationFactory::getSettings(bool configurable)
{
    return configurable ? _hostSettings : _hierarchySettings;
}

mv::DataTypes SmoothingTransformationFactory::supportedDataTypes() const
{
    return { PointType };
}

mv::gui::PluginTriggerActions SmoothingTransformationFactory::createTriggerActions(const mv::Datasets& datasets, bool configurable) const
{
    mv::gui::PluginTriggerActions pluginTriggerActions;

    auto factory = const_cast<SmoothingTransformationFactory*>(this);
    const auto menuName = configurable ? QString("Smoothing Transformation") : QString("Smoothing Transformation...");

    auto& settings = factory->getSettings(configurable);

    auto pluginTriggerAction = new mv::gui::PluginTriggerAction(factory, this,
        menuName, "Smooth each point's spectrum", icon(),
        [this, &settings, datasets, configurable](mv::gui::PluginTriggerAction& pluginTriggerAction) -> void {
            if (!configurable && !settings.edit())
                return;

            const auto targets = datasets.isEmpty() ? pluginTriggerAction.getDatasets() : datasets;

            int sgWindowSize = 0;
            int sgPolynomialOrder = 0;
            const auto movingAverageWindowSize = settings.getMovingAverageWindowSize();

            settings.getSavitzkyGolayParameters(sgWindowSize, sgPolynomialOrder);

            for (const auto& dataset : targets) {
                auto pluginInstance = dynamic_cast<SmoothingTransformation*>(plugins().requestPlugin(getKind()));
                if (pluginInstance) {
                    pluginInstance->setInputDataset(dataset);
                    pluginInstance->setKernel(settings.getKernel());
                    pluginInstance->setOutput(settings.getOutput());
                    pluginInstance->setMovingAverageWindowSize(movingAverageWindowSize);
                    pluginInstance->setSavitzkyGolayParameters(sgWindowSize, sgPolynomialOrder);
                    pluginInstance->transform();
                }
            }
        });

    pluginTriggerAction->setConfigurationAction(&settings);
    pluginTriggerActions << pluginTriggerAction;

    return pluginTriggerActions;
}

mv::gui::PluginTriggerActions SmoothingTransformationFactory::getPluginTriggerActions(const mv::Datasets& datasets) const
{
    if (datasets.count() < 1 || !PluginFactory::areAllDatasetsOfTheSameType(datasets, PointType))
        return {};

    return createTriggerActions(datasets, false);
}

mv::gui::PluginTriggerActions SmoothingTransformationFactory::getPluginTriggerActions(const mv::DataTypes& dataTypes) const
{
    if (dataTypes.isEmpty() || dataTypes.count(PointType) != dataTypes.count())
        return {};

    return createTriggerActions({}, true);
}