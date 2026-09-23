#include "heatsourcedetector.h"
#include "logger.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <QStack>

HeatSourceDetector::HeatSourceDetector(QObject *parent)
    : QObject(parent)
{
}

void HeatSourceDetector::setThresholdDelta(double delta)
{
    m_thresholdDelta = delta;
}

void HeatSourceDetector::setMinArea(int area)
{
    m_minArea = area;
}

void HeatSourceDetector::setMaxSources(int max)
{
    m_maxSources = max;
}

// 半径可调的腐蚀: radius 圈内的像素全为前景才保留(radius=1 -> 3x3, radius=2 -> 5x5)
void HeatSourceDetector::erodeN(const unsigned char *src, unsigned char *dst, int w, int h, int radius)
{
    std::memset(dst, 0, w * h);
    if (radius < 1) { std::memcpy(dst, src, w * h); return; }
    for (int y = radius; y < h - radius; y++) {
        for (int x = radius; x < w - radius; x++) {
            bool allOne = true;
            for (int dy = -radius; dy <= radius && allOne; dy++) {
                for (int dx = -radius; dx <= radius && allOne; dx++) {
                    if (!src[(y + dy) * w + (x + dx)]) allOne = false;
                }
            }
            dst[y * w + x] = allOne ? 255 : 0;
        }
    }
}

// 半径可调的膨胀
void HeatSourceDetector::dilateN(const unsigned char *src, unsigned char *dst, int w, int h, int radius)
{
    std::memset(dst, 0, w * h);
    if (radius < 1) { std::memcpy(dst, src, w * h); return; }
    for (int y = radius; y < h - radius; y++) {
        for (int x = radius; x < w - radius; x++) {
            bool hasOne = false;
            for (int dy = -radius; dy <= radius && !hasOne; dy++) {
                for (int dx = -radius; dx <= radius && !hasOne; dx++) {
                    if (src[(y + dy) * w + (x + dx)]) hasOne = true;
                }
            }
            dst[y * w + x] = hasOne ? 255 : 0;
        }
    }
}

// 开运算(先腐蚀后膨胀): 采用 2 像素半径(5x5)结构元, 用于滤除细小线条干扰。
// 典型场景: 热像仪画面四周存在一条亮(黄)色细边框, 其在温度矩阵中表现为高温细线,
// 经二值化后会与真实热源连成一片, 导致边界框沿图像外边界扩张。
// 半径 2 的腐蚀可彻底断开宽度 ≤4 像素的细线, 随后膨胀恢复目标原有尺寸(不改变真实热源轮廓)。
void HeatSourceDetector::morphOpen(unsigned char *binary, int width, int height)
{
    static const int OPEN_RADIUS = 2;   // 2 像素腐蚀再膨胀
    int size = width * height;
    unsigned char *temp = new unsigned char[size];
    erodeN(binary, temp, width, height, OPEN_RADIUS);
    dilateN(temp, binary, width, height, OPEN_RADIUS);
    delete[] temp;
}

// 闭运算(先膨胀后腐蚀): 填平目标内部小孔/裂缝, 使用 1 像素半径(3x3)保持细节
void HeatSourceDetector::morphClose(unsigned char *binary, int width, int height)
{
    int size = width * height;
    unsigned char *temp = new unsigned char[size];
    dilateN(binary, temp, width, height, 1);
    erodeN(temp, binary, width, height, 1);
    delete[] temp;
}

QVector<HeatSource> HeatSourceDetector::connectedComponents(const float *tempData,
                                                             const unsigned char *binary,
                                                             int width, int height,
                                                             int minArea)
{
    QVector<HeatSource> sources;
    int size = width * height;
    int *labels = new int[size];
    std::memset(labels, 0, size * sizeof(int));

    int currentLabel = 0;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            if (binary[idx] && labels[idx] == 0) {
                // BFS flood fill
                currentLabel++;
                QStack<int> stack;
                stack.push(idx);
                labels[idx] = currentLabel;

                int minX = x, maxX = x, minY = y, maxY = y;
                float sumTemp = 0;
                float maxT = tempData[idx];
                int pixelCount = 0;

                while (!stack.isEmpty()) {
                    int ci = stack.pop();
                    int cx = ci % width;
                    int cy = ci / width;
                    pixelCount++;
                    sumTemp += tempData[ci];
                    if (tempData[ci] > maxT) maxT = tempData[ci];
                    if (cx < minX) minX = cx;
                    if (cx > maxX) maxX = cx;
                    if (cy < minY) minY = cy;
                    if (cy > maxY) maxY = cy;

                    // 4-connectivity neighbors
                    const int dx4[] = {-1, 1, 0, 0};
                    const int dy4[] = {0, 0, -1, 1};
                    for (int d = 0; d < 4; d++) {
                        int nx = cx + dx4[d];
                        int ny = cy + dy4[d];
                        if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                            int ni = ny * width + nx;
                            if (binary[ni] && labels[ni] == 0) {
                                labels[ni] = currentLabel;
                                stack.push(ni);
                            }
                        }
                    }
                }

                if (pixelCount >= minArea) {
                    HeatSource hs;
                    hs.maxTemp = maxT;
                    hs.avgTemp = sumTemp / pixelCount;
                    hs.centerX = (minX + maxX) / 2;
                    hs.centerY = (minY + maxY) / 2;
                    hs.boundingBoxX = minX;
                    hs.boundingBoxY = minY;
                    hs.boundingBoxW = maxX - minX + 1;
                    hs.boundingBoxH = maxY - minY + 1;
                    hs.area = static_cast<float>(pixelCount);
                    sources.append(hs);
                }
            }
        }
    }

    delete[] labels;

    // Sort by area descending, keep top N
    std::sort(sources.begin(), sources.end(),
              [](const HeatSource &a, const HeatSource &b) { return a.area > b.area; });
    while (sources.size() > m_maxSources) {
        sources.removeLast();
    }

    return sources;
}

QVector<HeatSource> HeatSourceDetector::detect(const float *tempData, int width, int height, float envTemp)
{
    int size = width * height;

    // 模式决定筛选参数:
    //   自动: 温度阈值 = 环境温度 + 差值阈值; 面积阈值 = m_minArea
    //   手动: 温度阈值 = 用户设定的最低温度;   面积阈值 = 用户设定的最小面积像素
    const float threshold = (m_mode == Manual)
                                ? static_cast<float>(m_manualMinTemp)
                                : (envTemp + static_cast<float>(m_thresholdDelta));
    const int minArea = (m_mode == Manual) ? m_manualMinArea : m_minArea;

    // Step 1: Binarize
    unsigned char *binary = new unsigned char[size];
    for (int i = 0; i < size; i++) {
        binary[i] = (tempData[i] > threshold) ? 255 : 0;
    }

    // Step 2: Morphological operations
    morphOpen(binary, width, height);   // Remove noise
    morphClose(binary, width, height);  // Fill gaps

    // Step 3: Connected component analysis
    QVector<HeatSource> sources = connectedComponents(tempData, binary, width, height, minArea);

    delete[] binary;

    emit heatSourcesDetected(sources);
    return sources;
}

HeatSource HeatSourceDetector::findHottest(const QVector<HeatSource> &sources) const
{
    HeatSource hottest = {};
    if (sources.isEmpty()) return hottest;

    hottest = sources[0];
    for (const auto &s : sources) {
        if (s.maxTemp > hottest.maxTemp) {
            hottest = s;
        }
    }
    return hottest;
}

HeatSource HeatSourceDetector::selectTarget(const QVector<HeatSource> &sources) const
{
    HeatSource target = {};
    if (sources.isEmpty()) return target;

    // 归一化基准: 最亮温度 与 最大面积
    double maxTemp = sources[0].maxTemp;
    double maxArea = sources[0].area;
    for (const auto &s : sources) {
        if (s.maxTemp > maxTemp) maxTemp = s.maxTemp;
        if (s.area > maxArea) maxArea = s.area;
    }
    if (maxTemp <= 0.0) maxTemp = 1.0;
    if (maxArea <= 0.0) maxArea = 1.0;

    // 评分 = 亮度归一化 * 权重 + 面积归一化 * 权重
    // 这样"又亮又大"的热源优先被选为目标; 亮度相同则面积大者优先
    double bestScore = -1.0;
    for (const auto &s : sources) {
        const double brightNorm = static_cast<double>(s.maxTemp) / maxTemp;
        const double areaNorm = static_cast<double>(s.area) / maxArea;
        const double score = m_weightBright * brightNorm + m_weightArea * areaNorm;
        if (score > bestScore) {
            bestScore = score;
            target = s;
        }
    }

    LOG_DEBUG(QString("selectTarget: 目标 中心(%1,%2) 温度=%3 面积=%4px 评分=%5")
                  .arg(target.centerX).arg(target.centerY)
                  .arg(target.maxTemp, 0, 'f', 1).arg(target.area)
                  .arg(bestScore, 0, 'f', 3));
    return target;
}
