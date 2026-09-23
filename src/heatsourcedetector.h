#ifndef HEATSOURCEDETECTOR_H
#define HEATSOURCEDETECTOR_H

#include <QObject>
#include <QVector>
#include <QMutex>

struct HeatSource {
    float maxTemp;        // 最高温度
    float avgTemp;        // 平均温度
    int centerX;          // 中心X像素坐标
    int centerY;          // 中心Y像素坐标
    int boundingBoxX;     // 边界框X
    int boundingBoxY;     // 边界框Y
    int boundingBoxW;     // 边界框宽度
    int boundingBoxH;     // 边界框高度
    float area;           // 面积（像素数）
};

class HeatSourceDetector : public QObject
{
    Q_OBJECT

public:
    // 检测/筛选模式
    enum SelectMode {
        AutoBrightArea = 0,   // 自动: 优先选择"最亮且面积最大"的热源(亮度+面积加权评分)
        Manual = 1            // 手动: 按 最低温度 与 最小面积像素 作为筛选参数
    };

    explicit HeatSourceDetector(QObject *parent = nullptr);

    // 设置检测参数（自动模式使用：高于环境温度的差值阈值）
    void setThresholdDelta(double delta);
    void setMinArea(int area);
    void setMaxSources(int max);

    // 模式与手动筛选参数
    void setSelectMode(SelectMode mode) { m_mode = mode; }
    SelectMode selectMode() const { return m_mode; }
    void setManualMinTemp(double t) { m_manualMinTemp = t; }
    void setManualMinArea(int px) { m_manualMinArea = px; }
    double manualMinTemp() const { return m_manualMinTemp; }
    int manualMinArea() const { return m_manualMinArea; }

    // 自动模式加权系数（亮度权重 / 面积权重, 默认 0.5/0.5）
    void setAutoWeights(double brightWeight, double areaWeight)
    {
        m_weightBright = brightWeight;
        m_weightArea = areaWeight;
    }

    // 核心检测函数：输入温度矩阵（row-major float数组）和尺寸
    QVector<HeatSource> detect(const float *tempData, int width, int height,
                               float envTemp = 25.0f);

    // 获取最大热源（最热的）
    HeatSource findHottest(const QVector<HeatSource> &sources) const;

    // 自动模式目标选择: 对"亮度"与"面积"归一化后加权评分, 取得分最高者
    HeatSource selectTarget(const QVector<HeatSource> &sources) const;

signals:
    void heatSourcesDetected(const QVector<HeatSource> &sources);

private:
    // 形态学开运算去噪(2 像素半径 / 5x5, 去除画面四周与目标相连的细小线条干扰)
    void morphOpen(unsigned char *binary, int width, int height);
    // 形态学闭运算填充
    void morphClose(unsigned char *binary, int width, int height);
    // 连通域分析
    QVector<HeatSource> connectedComponents(const float *tempData,
                                             const unsigned char *binary,
                                             int width, int height,
                                             int minArea);
    // 半径可调的膨胀(radius=1 即 3x3, radius=2 即 5x5)
    void dilateN(const unsigned char *src, unsigned char *dst, int w, int h, int radius);
    // 半径可调的腐蚀
    void erodeN(const unsigned char *src, unsigned char *dst, int w, int h, int radius);

    double m_thresholdDelta = 10.0;  // 高于环境温度的阈值（自动模式）
    int m_minArea = 50;              // 最小面积（自动模式）
    int m_maxSources = 5;            // 最大热源数

    SelectMode m_mode = AutoBrightArea;  // 默认自动模式
    double m_manualMinTemp = 40.0;       // 手动模式: 最低温度 (°C)
    int m_manualMinArea = 50;            // 手动模式: 最小面积 (像素)
    double m_weightBright = 0.5;         // 自动评分: 亮度权重
    double m_weightArea = 0.5;           // 自动评分: 面积权重
};

#endif // HEATSOURCEDETECTOR_H
