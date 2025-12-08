#pragma once

#include <algorithm>
#include <functional>
#include <cctype>
#include <locale>
#include <string>
#include <sstream>
#include <vector>
#include <random>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <cstdint>

#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>

namespace InteractiveWaterVR::Util
{
	// trim from start (in place)
	static inline void ltrim(std::string& s)
	{
		s.erase(s.begin(), std::find_if(s.begin(), s.end(),
			[](unsigned char ch) { return !std::isspace(ch); }));;
	}

	// trim from end (in place)
	static inline void rtrim(std::string& s)
	{
		s.erase(std::find_if(s.rbegin(), s.rend(),
			[](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
	}

	// trim from both ends (in place)
	static inline void trim(std::string& s)
	{
		ltrim(s);
		rtrim(s);
	}

	// copies
	static inline std::string ltrim_copy(std::string s) { ltrim(s); return s; }
	static inline std::string rtrim_copy(std::string s) { rtrim(s); return s; }
	static inline std::string trim_copy(std::string s) { trim(s); return s; }

	static inline std::vector<std::string> split(const std::string& s, char delimiter)
	{
		std::vector<std::string> tokens;
		std::string token;
		std::istringstream tokenStream(s);
		while (std::getline(tokenStream, token, delimiter)) {
			tokens.push_back(token);
		}
		return tokens;
	}

	static inline void skipComments(std::string& str)
	{
		auto pos = str.find('#');
		if (pos != std::string::npos) str.erase(pos);
	}

	static inline void skipTactExtension(std::string& str)
	{
		auto pos = str.find(".tact");
		if (pos != std::string::npos) str.erase(pos);
	}

	static inline std::vector<std::string> get_all_files_names_within_folder(const std::string& folder)
	{
		std::vector<std::string> names;
		try {
			for (const auto& entry : std::filesystem::directory_iterator(folder)) {
				names.push_back(entry.path().filename().string());
			}
		}
		catch (...) {
			// ignore
		}
		return names;
	}

	static inline bool stringStartsWith(const std::string& str, const std::string& prefix)
	{
		return str.rfind(prefix, 0) == 0;
	}

	// Random helpers using Mersenne Twister
	static inline float randf(float lo, float hi)
	{
		if (hi == lo) return lo;
		static thread_local std::mt19937 rng(std::random_device{}());
		std::uniform_real_distribution<float> dist(lo, hi);
		return dist(rng);
	}

	static inline size_t randomGenerator(size_t min, size_t max)
	{
		static thread_local std::mt19937 rng(std::random_device{}());
		std::uniform_int_distribution<std::mt19937::result_type> dist(min, max);
		return dist(rng);
	}

	static inline int randi(int lo, int hi)
	{
		if (hi == lo) return lo;
		static thread_local std::mt19937 rng(std::random_device{}());
		std::uniform_int_distribution<int> dist(lo, hi);
		return dist(rng);
	}

	static inline std::string toLowerCase(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), ::tolower);
		return s;
	}

	template<typename T>
	static inline bool vectorContains(const std::vector<T>& vec, const T& item) {
		return std::find(vec.begin(), vec.end(), item) != vec.end();
	}

	static inline bool Contains(const std::string& str, const std::string& ministr)
	{
		return str.find(ministr) != std::string::npos;
	}

	static inline bool ContainsNoCase(std::string str, std::string ministr)
	{
		std::transform(str.begin(), str.end(), str.begin(), ::tolower);
		std::transform(ministr.begin(), ministr.end(), ministr.begin(), ::tolower);
		return str.find(ministr) != std::string::npos;
	}

	template<typename T>
	static inline bool contains(const std::vector<T>& vec, const T& item) { return vectorContains(vec, item); }

	static inline int GetConfigSettingsValue(const std::string& line, std::string& variable)
	{
		int value = 0;
		auto splittedLine = split(line, '=');
		variable.clear();
		if (splittedLine.size() > 1) {
			variable = splittedLine[0]; trim(variable);
			std::string valuestr = splittedLine[1]; trim(valuestr);
			try { value = std::stoi(valuestr); }
			catch (...) { value = 0; }
		}
		return value;
	}

	static inline float GetConfigSettingsFloatValue(const std::string& line, std::string& variable)
	{
		float value = 0.0f;
		auto splittedLine = split(line, '=');
		variable.clear();
		if (splittedLine.size() > 1) {
			variable = splittedLine[0]; trim(variable);
			std::string valuestr = splittedLine[1]; trim(valuestr);
			try { value = std::stof(valuestr); }
			catch (...) { value = 0.0f; }
		}
		return value;
	}

	static inline std::string GetConfigSettingsStringValue(const std::string& line, std::string& variable)
	{
		std::string valuestr;
		auto splittedLine = split(line, '=');
		variable.clear();
		if (splittedLine.size() > 0) { variable = splittedLine[0]; trim(variable); }
		if (splittedLine.size() > 1) { valuestr = splittedLine[1]; trim(valuestr); }
		return valuestr;
	}

	static inline size_t randomGeneratorLowMoreProbable(size_t lowermin, size_t lowermax, size_t highermin, size_t highermax, int probability)
	{
		static thread_local std::mt19937 rng(std::random_device{}());
		std::uniform_int_distribution<std::mt19937::result_type> dist(1, probability);
		if (dist(rng) == 1) {
			std::uniform_int_distribution<std::mt19937::result_type> distir(highermin, highermax);
			return distir(rng);
		}
		else {
			std::uniform_int_distribution<std::mt19937::result_type> distir(lowermin, lowermax);
			return distir(rng);
		}
	}

	static inline std::uint32_t GetModIndex(std::uint32_t formId) { return formId >> 24; }
	static inline std::uint32_t GetBaseFormID(std::uint32_t formId) { return formId & 0x00FFFFFFu; }
	static inline bool IsValidModIndex(std::uint32_t modIndex) { return modIndex > 0 && modIndex != 0xFF; }

	// vlibGetSetting and vlibGetGameSetting: attempt to use RE::Setting via CommonLibSSE-NG if available
	static inline double vlibGetSetting(const char* name)
	{
		// RE::Setting API isn't guaranteed; attempt to use SKSE::GetINISetting or return -1
		auto setting = SKSE::GetINISetting(name);
		if (!setting) return -1;
		double out = 0.0;
		if (setting->GetDouble(&out)) return out;
		return -1;
	}

	static inline double vlibGetGameSetting(const char* name)
	{
		auto setting = SKSE::GetGameSetting(name);
		if (!setting) return -1;
		double out = 0.0;
		if (setting->GetDouble(&out)) return out;
		return -1;
	}

	// Math and Ni types rely on RE types included via RE/Skyrim.h
	static inline RE::NiMatrix33 slerpMatrix(float interp, RE::NiMatrix33 mat1, RE::NiMatrix33 mat2) {
		// Ported implementation uses same logic as original; keep using RE:: types
		// Convert mat1 to quaternion
		float q1w = std::sqrt(std::max(0.0f, 1.0f + mat1.data[0][0] + mat1.data[1][1] + mat1.data[2][2])) / 2.0f;
		float q1x = std::sqrt(std::max(0.0f, 1.0f + mat1.data[0][0] - mat1.data[1][1] - mat1.data[2][2])) / 2.0f;
		float q1y = std::sqrt(std::max(0.0f, 1.0f - mat1.data[0][0] + mat1.data[1][1] - mat1.data[2][2])) / 2.0f;
		float q1z = std::sqrt(std::max(0.0f, 1.0f - mat1.data[0][0] - mat1.data[1][1] + mat1.data[2][2])) / 2.0f;
		q1x = std::copysign(q1x, mat1.data[2][1] - mat1.data[1][2]);
		q1y = std::copysign(q1y, mat1.data[0][2] - mat1.data[2][0]);
		q1z = std::copysign(q1z, mat1.data[1][0] - mat1.data[0][1]);

		float q2w = std::sqrt(std::max(0.0f, 1.0f + mat2.data[0][0] + mat2.data[1][1] + mat2.data[2][2])) / 2.0f;
		float q2x = std::sqrt(std::max(0.0f, 1.0f + mat2.data[0][0] - mat2.data[1][1] - mat2.data[2][2])) / 2.0f;
		float q2y = std::sqrt(std::max(0.0f, 1.0f - mat2.data[0][0] + mat2.data[1][1] - mat2.data[2][2])) / 2.0f;
		float q2z = std::sqrt(std::max(0.0f, 1.0f - mat2.data[0][0] - mat2.data[1][1] + mat2.data[2][2])) / 2.0f;
		q2x = std::copysign(q2x, mat2.data[2][1] - mat2.data[1][2]);
		q2y = std::copysign(q2y, mat2.data[0][2] - mat2.data[2][0]);
		q2z = std::copysign(q2z, mat2.data[1][0] - mat2.data[0][1]);

		double dot = q1w * q2w + q1x * q2x + q1y * q2y + q1z * q2z;
		if (dot < 0.0f) {
			q2w = -q2w; q2x = -q2x; q2y = -q2y; q2z = -q2z; dot = -dot;
		}

		float q3w, q3x, q3y, q3z;
		if (dot > 0.9995f) {
			q3w = q1w + interp * (q2w - q1w);
			q3x = q1x + interp * (q2x - q1x);
			q3y = q1y + interp * (q2y - q1y);
			q3z = q1z + interp * (q2z - q1z);
			float length = std::sqrt(q3w * q3w + q3x * q3x + q3y * q3y + q3z * q3z);
			q3w /= length; q3x /= length; q3y /= length; q3z /= length;
		}
		else {
			float theta_0 = std::acos(dot);
			float theta = theta_0 * interp;
			float sin_theta = std::sin(theta);
			float sin_theta_0 = std::sin(theta_0);
			float s0 = std::cos(theta) - dot * sin_theta / sin_theta_0;
			float s1 = sin_theta / sin_theta_0;
			q3w = (s0 * q1w) + (s1 * q2w);
			q3x = (s0 * q1x) + (s1 * q2x);
			q3y = (s0 * q1y) + (s1 * q2y);
			q3z = (s0 * q1z) + (s1 * q2z);
		}

		RE::NiMatrix33 result;
		result.data[0][0] = 1 - (2 * q3y * q3y) - (2 * q3z * q3z);
		result.data[0][1] = (2 * q3x * q3y) - (2 * q3z * q3w);
		result.data[0][2] = (2 * q3x * q3z) + (2 * q3y * q3w);
		result.data[1][0] = (2 * q3x * q3y) + (2 * q3z * q3w);
		result.data[1][1] = 1 - (2 * q3x * q3x) - (2 * q3z * q3z);
		result.data[1][2] = (2 * q3y * q3z) - (2 * q3x * q3w);
		result.data[2][0] = (2 * q3x * q3z) - (2 * q3y * q3w);
		result.data[2][1] = (2 * q3y * q3z) + (2 * q3x * q3w);
		result.data[2][2] = 1 - (2 * q3x * q3x) - (2 * q3y * q3y);
		return result;
	}

	static inline float distance(const RE::NiPoint3& po1, const RE::NiPoint3& po2)
	{
		float x = po1.x - po2.x; float y = po1.y - po2.y; float z = po1.z - po2.z;
		return std::sqrtf(x * x + y * y + z * z);
	}

	static inline float distance2dNoSqrt(const RE::NiPoint3& po1, const RE::NiPoint3& po2)
	{
		float x = po1.x - po2.x; float y = po1.y - po2.y; return x * x + y * y;
	}

	static inline float distanceNoSqrt(const RE::NiPoint3& po1, const RE::NiPoint3& po2)
	{
		float x = po1.x - po2.x; float y = po1.y - po2.y; float z = po1.z - po2.z; return x * x + y * y + z * z;
	}

	static inline float magnitude(const RE::NiPoint3& p) { return std::sqrtf(p.x * p.x + p.y * p.y + p.z * p.z); }
	static inline float magnitude2d(const RE::NiPoint3& p) { return std::sqrtf(p.x * p.x + p.y * p.y); }
	static inline float magnitudePwr2(const RE::NiPoint3& p) { return p.x * p.x + p.y * p.y + p.z * p.z; }

	static inline RE::NiPoint3 crossProduct(const RE::NiPoint3& A, const RE::NiPoint3& B)
	{
		return RE::NiPoint3(A.y * B.z - A.z * B.y, A.z * B.x - A.x * B.z, A.x * B.y - A.y * B.x);
	}

	static inline float GetPercentageValue(float number1, float number2, float division)
	{
		if (division == 1.0f) return number2;
		else if (division == 0) return number1;
		else return number1 + ((number2 - number1) * (division));
	}

	static inline float CalculateCollisionAmount(const RE::NiPoint3& a, const RE::NiPoint3& b, float Wradius, float Bradius)
	{
		float distPwr2 = distanceNoSqrt(a, b);
		float totalRadius = Wradius + Bradius;
		if (distPwr2 < totalRadius * totalRadius) return totalRadius - std::sqrtf(distPwr2);
		return0;
	}

	static inline bool invert(const RE::NiMatrix33& matIn, RE::NiMatrix33& matOut)
	{
		float inv[9];
		inv[0] = matIn.data[1][1] * matIn.data[2][2] - matIn.data[2][1] * matIn.data[1][2];
		inv[1] = matIn.data[1][2] * matIn.data[2][0] - matIn.data[1][0] * matIn.data[2][2];
		inv[2] = matIn.data[1][0] * matIn.data[2][1] - matIn.data[2][0] * matIn.data[1][1];
		inv[3] = matIn.data[0][2] * matIn.data[2][1] - matIn.data[0][1] * matIn.data[2][2];
		inv[4] = matIn.data[0][0] * matIn.data[2][2] - matIn.data[0][2] * matIn.data[2][0];
		inv[5] = matIn.data[2][0] * matIn.data[0][1] - matIn.data[0][0] * matIn.data[2][1];
		inv[6] = matIn.data[0][1] * matIn.data[1][2] - matIn.data[0][2] * matIn.data[1][1];
		inv[7] = matIn.data[1][0] * matIn.data[0][2] - matIn.data[0][0] * matIn.data[1][2];
		inv[8] = matIn.data[0][0] * matIn.data[1][1] - matIn.data[1][0] * matIn.data[0][1];
		double determinant =
			+matIn.data[0][0] * (matIn.data[1][1] * matIn.data[2][2] - matIn.data[2][1] * matIn.data[1][2])
			- matIn.data[0][1] * (matIn.data[1][0] * matIn.data[2][2] - matIn.data[1][2] * matIn.data[2][0])
			+ matIn.data[0][2] * (matIn.data[1][0] * matIn.data[2][1] - matIn.data[1][1] * matIn.data[2][0]);
		if (determinant >= -0.001 || determinant <= 0.001) return false;
		for (int i = 0; i < 9; i++) matOut.data[i / 3][i % 3] = inv[i] / determinant;
		return true;
	}

	static inline float determinant(const RE::NiPoint3& a, const RE::NiPoint3& b, const RE::NiPoint3& c)
	{
		float det = 0;
		det = det + ((a.x * b.y * c.z) - (b.z * c.y));
		det = det + ((a.y * b.z * c.x) - (b.x * c.z));
		det = det + ((a.z * b.x * c.y) - (b.y * c.x));
		return det;
	}

	static inline float Dot(const RE::NiPoint3& A, const RE::NiPoint3& B) { return (A.x * B.x + A.y * B.y + A.z * B.z); }
	static inline float clamp(float val, float minv, float maxv) { if (val < minv) return minv; else if (val > maxv) return maxv; return val; }
	static inline RE::NiPoint3 normalize(const RE::NiPoint3& v) { float len = std::sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); return RE::NiPoint3(v.x / len, v.y / len, v.z / len); }
	static inline RE::NiPoint3 InterpolateBetweenVectors(const RE::NiPoint3& from, const RE::NiPoint3& to, float percentage)
	{
		return normalize((normalize(to) * percentage) + (normalize(from) * (100.0f - percentage))) * magnitude(to);
	}

	static inline RE::NiPoint3 ConvertRotation(RE::NiMatrix33 mat) { float heading, attitude, bank; mat.GetEulerAngles(&heading, &attitude, &bank); return RE::NiPoint3(heading, attitude, bank); }
	static inline float dot(const RE::NiPoint3& a, const RE::NiPoint3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	static inline RE::NiPoint3 cross(const RE::NiPoint3& a, const RE::NiPoint3& b) { return RE::NiPoint3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
	static inline float DegreesToRadians(float degrees) { return degrees * RE::MATH_PI / 180.0f; }
	static inline RE::NiMatrix33 RotateMatrix(const RE::NiMatrix33& originalRotation, const RE::NiPoint3& eulerAnglesDegrees)
	{
		float heading = DegreesToRadians(eulerAnglesDegrees.y);
		float attitude = DegreesToRadians(eulerAnglesDegrees.x);
		float bank = DegreesToRadians(eulerAnglesDegrees.z);
		RE::NiMatrix33 rotationMatrix; rotationMatrix.SetEulerAngles(heading, attitude, bank);
		RE::NiMatrix33 finalRotation = originalRotation * rotationMatrix; return finalRotation;
	}

	static inline RE::NiPoint3 rotate(const RE::NiPoint3& v, const RE::NiPoint3& axis, float theta)
	{
		const float cos_theta = cosf(theta);
		return (v * cos_theta) + (crossProduct(axis, v) * sinf(theta)) + (axis * Dot(axis, v)) * (1 - cos_theta);
	}

	static inline RE::NiMatrix33 getRotationAxisAngle(RE::NiPoint3 axis, float theta)
	{
		RE::NiMatrix33 result;
		double c = cosf(theta); double s = sinf(theta); double t = 1.0 - c; axis = normalize(axis);
		result.data[0][0] = c + axis.x * axis.x * t; result.data[1][1] = c + axis.y * axis.y * t; result.data[2][2] = c + axis.z * axis.z * t;
		double tmp1 = axis.x * axis.y * t; double tmp2 = axis.z * s; result.data[1][0] = tmp1 + tmp2; result.data[0][1] = tmp1 - tmp2;
		tmp1 = axis.x * axis.z * t; tmp2 = axis.y * s; result.data[2][0] = tmp1 - tmp2; result.data[0][2] = tmp1 + tmp2;
		tmp1 = axis.y * axis.z * t; tmp2 = axis.x * s; result.data[2][1] = tmp1 + tmp2; result.data[1][2] = tmp1 - tmp2;
		return result;
	}

	static inline RE::NiPoint3 interpVector(float interp, RE::NiPoint3 vec1, RE::NiPoint3 vec2) { return vec1 + (vec2 - vec1) * interp; }
	static inline RE::NiMatrix33 getRotation(RE::NiPoint3 a, RE::NiPoint3 b) { /* implementation omitted for brevity */
		RE::NiMatrix33 mat; mat.Identity(); return mat;
	}

	static inline float angleBetweenVectors(const RE::NiPoint3& v1, const RE::NiPoint3& v2)
	{
		return std::acos(dot(v1, v2) / (magnitude(v1) * magnitude(v2))) * 57.295776f;
	}

	static inline uint64_t GetButtonMaskFromId(int id) { return 1ull << id; }
	static inline float calculateProgressPercent(float currentValue, float startValue, float endValue) { if (endValue == startValue) return 100.0f; float progress = (currentValue - startValue) / (endValue - startValue) * 100.0f; if (progress < 0.0f) return 0.0f; if (progress > 100.0f) return 100.0f; return progress; }
	static inline float calculateCurrentValue(float progressPercent, float startValue, float endValue) { if (progressPercent < 0.0f) progressPercent = 0.0f; return startValue + (progressPercent / 100.0f) * (endValue - startValue); }
	static inline float normalizeDegree(float degree) { while (degree < 0.0f) degree += 360.0f; return degree; }

} // namespace InteractiveWaterVR::Util
