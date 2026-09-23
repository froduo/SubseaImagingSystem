#ifndef ANGLECALCULATOR_H
#define ANGLECALCULATOR_H

#include <QObject>

struct AngleDifference {
    float deltaX;         // X方向角度差（度），正=右偏
    float deltaY;         // Y方向角度差（度），正=上偏
    float distance;       // 偏离中心的总角度
    bool valid;           // 是否有效（检测到热源）
    int sourceX;          // 热源像素X
    int sourceY;          // 热源像素Y
    float maxTemp;        // 最高温度
};

class AngleCalculator : public QObject
{
    Q_OBJECT

public:
    explicit AngleCalculator(QObject *parent = nullptr);

    // 设置FOV参数
    void setFOV(double hfov, double vfov);
    void setImageSize(int width, int height);

    // 计算角度差
    AngleDifference calculate(int sourceX, int sourceY, float maxTemp);
    AngleDifference calculateInvalid();  // 返回无效结果

    // Getters
    double degPerPixelH() const { return m_degPerPixelH; }
    double degPerPixelV() const { return m_degPerPixelV; }

signals:
    void angleCalculated(const AngleDifference &angle);

private:
    double m_hfov = 24.2;        // 水平视场角
    double m_vfov = 19.4;        // 垂直视场角
    int m_imageWidth = 640;
    int m_imageHeight = 512;
    int m_centerX = 320;
    int m_centerY = 256;
    double m_degPerPixelH = 24.2 / 640.0;  // ~0.0378125
    double m_degPerPixelV = 19.4 / 512.0;  // ~0.037890625
};

#endif // ANGLECALCULATOR_H
