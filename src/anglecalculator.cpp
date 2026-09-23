#include "anglecalculator.h"
#include <cmath>

AngleCalculator::AngleCalculator(QObject *parent)
    : QObject(parent)
{
}

void AngleCalculator::setFOV(double hfov, double vfov)
{
    m_hfov = hfov;
    m_vfov = vfov;
    m_degPerPixelH = m_hfov / m_imageWidth;
    m_degPerPixelV = m_vfov / m_imageHeight;
}

void AngleCalculator::setImageSize(int width, int height)
{
    m_imageWidth = width;
    m_imageHeight = height;
    m_centerX = width / 2;
    m_centerY = height / 2;
    m_degPerPixelH = m_hfov / m_imageWidth;
    m_degPerPixelV = m_vfov / m_imageHeight;
}

AngleDifference AngleCalculator::calculate(int sourceX, int sourceY, float maxTemp)
{
    AngleDifference result;
    result.sourceX = sourceX;
    result.sourceY = sourceY;
    result.maxTemp = maxTemp;
    result.valid = true;

    // 角度差 = (热源中心 - 图像中心) * 每像素角度
    result.deltaX = static_cast<float>((sourceX - m_centerX) * m_degPerPixelH);
    result.deltaY = static_cast<float>((m_centerY - sourceY) * m_degPerPixelV); // Y轴翻转（图像Y向下，角度Y向上）
    result.distance = std::sqrt(result.deltaX * result.deltaX + result.deltaY * result.deltaY);

    emit angleCalculated(result);
    return result;
}

AngleDifference AngleCalculator::calculateInvalid()
{
    AngleDifference result;
    result.deltaX = 0;
    result.deltaY = 0;
    result.distance = 0;
    result.valid = false;
    result.sourceX = 0;
    result.sourceY = 0;
    result.maxTemp = 0;
    return result;
}
