#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace Magpie {

struct ParameterPanelAnchor {
	ImGuiID id = 0;
	float fraction = 0;
};

inline void ParameterHeading(const char* text) {
	if (ImGui::CalcTextSize(text).x + ImGui::GetStyle().SeparatorTextPadding.x * 2 <= ImGui::GetContentRegionAvail().x) ImGui::SeparatorText(text);
	else { ImGui::TextWrapped("%s", text); ImGui::Separator(); }
}

inline bool ParameterCheckbox(const char* label, bool* value) {
	const float width = std::max(ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x -
		ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
	const char* cursor = label;
	const char* end = label + std::strlen(label);
	std::string wrapped;
	while (cursor < end) {
		const char* stop = ImGui::GetFont()->CalcWordWrapPositionA(
			ImGui::GetFontSize() / ImGui::GetFont()->FontSize, cursor, end, width);
		if (stop <= cursor) stop = end;
		wrapped.append(cursor, stop);
		cursor = stop;
		while (cursor < end && (*cursor == ' ' || *cursor == '\n')) ++cursor;
		if (cursor < end) wrapped += '\n';
	}
	// A constant ID suffix keeps keyboard/drag state independent of wrapping.
	wrapped += "###"; wrapped += label;
	return ImGui::Checkbox(wrapped.c_str(), value);
}

// Button labels remain readable when a narrow viewport and a large font meet.
inline float ParameterButtonHeight(const char* label, float width) {
	const auto padding = ImGui::GetStyle().FramePadding;
	return ImGui::CalcTextSize(label, nullptr, true, std::max(1.f, width - padding.x * 2)).y + padding.y * 2;
}

inline bool ParameterButton(const char* label, float width) {
	width = std::max(1.f, width);
	const auto padding = ImGui::GetStyle().FramePadding;
	if (ImGui::CalcTextSize(label).x + padding.x * 2 <= width) return ImGui::Button(label, {width, 0});
	const float height = ParameterButtonHeight(label, width);
	const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
	const bool pressed = ImGui::Button(label, {width, height});
	ImGui::PopStyleColor();
	const auto position = ImGui::GetItemRectMin();
	ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
		{position.x + padding.x, position.y + padding.y}, textColor, label, nullptr,
		std::max(1.f, width - padding.x * 2));
	return pressed;
}

struct ParameterPanelFooterLayout {
	float width, revertWidth, applyWidth, contentHeight;
	bool inlineButtons, scrollWithContent;
	ParameterPanelFooterLayout(const char* revert, const char* apply, float scale, float statusHeight) {
		const auto available = ImGui::GetContentRegionAvail();
		const auto& style = ImGui::GetStyle();
		width = std::max(1.f, available.x);
		revertWidth = ImGui::CalcTextSize(revert).x + style.FramePadding.x * 2;
		applyWidth = ImGui::CalcTextSize(apply).x + style.FramePadding.x * 2;
		inlineButtons = revertWidth + applyWidth + style.ItemSpacing.x <= width;
		const float actionsHeight = inlineButtons ? ImGui::GetFrameHeight() :
			ParameterButtonHeight(revert, width) + ParameterButtonHeight(apply, width) + style.ItemSpacing.y;
		const float footerHeight = actionsHeight + statusHeight + style.ItemSpacing.y * 5;
		scrollWithContent = available.y - footerHeight < 120.f * scale;
		if (scrollWithContent) {
			width = std::max(1.f, available.x - style.ScrollbarSize);
			inlineButtons = revertWidth + applyWidth + style.ItemSpacing.x <= width;
		}
		contentHeight = std::max(1.f, available.y - (scrollWithContent ? 0 : footerHeight));
	}
};

// Each section is a vertical block. Its parameters use as many columns as the
// width permits; viewport height never participates in placement. Rendering
// uses the existing ID stack, so a width/zoom change cannot change control IDs.
class ParameterPanelLayout {
public:
	ParameterPanelLayout(float width, float scale, ParameterPanelAnchor restore = {})
		: _window(ImGui::GetCurrentWindow()), _origin(ImGui::GetCursorPos()),
		_width(std::max(1.f, width)), _scale(scale), _gap(24.f * scale), _restore(restore),
		_workRight(_window->WorkRect.Max.x), _contentRight(_window->ContentRegionRect.Max.x) {
		Region(0, _width);
	}

	void Section(const char* id) {
		EndSection();
		Group(id);
	}
	void Group(const char* id) {
		End(); FlushRow();
		Begin(id, _x, _y, _regionWidth, true);
	}
	void Item(const char* id) {
		End();
		if (_column == _columns) FlushRow();
		Begin(id, _x + _column * (_columnWidth + _gap), _y, _columnWidth, false);
	}
	bool Common(const char* id) {
		End(); FlushRow();
		_commonTop = _y;
		_commonSide = _width >= (260.f + 24.f + 280.f) * _scale;
		Begin(id, 0, _y, _commonSide ? 260.f * _scale : _width, true);
		return !_commonSide;
	}
	void EndCommon() {
		End();
		_reservedBottom = _y;
		if (_commonSide) {
			Region(260.f * _scale + _gap, _width - 260.f * _scale - _gap);
			_y = _commonTop;
		}
	}
	void End() {
		if (!_open) return;
		ImGui::EndGroup(); ImGui::PopTextWrapPos();
		const auto size = ImGui::GetItemRectSize();
		const auto position = ImGui::GetItemRectMin();
		const auto mouse = ImGui::GetIO().MousePos;
		if (_restore.id == _id) {
			const float anchorY = position.y + std::clamp(_restore.fraction, 0.f, 1.f) * size.y;
			ImGui::SetScrollY(_window->Scroll.y + anchorY - mouse.y);
		}
		if (_window->ClipRect.Contains(mouse) && ImRect(position,
			{position.x + _blockWidth, position.y + size.y}).Contains(mouse)) {
			hovered = {_id, std::clamp((mouse.y - position.y) / std::max(1.f, size.y), 0.f, 1.f)};
		}
		const float height = size.y + ImGui::GetStyle().ItemSpacing.y;
		if (_span) _y += height;
		else { _rowHeight = std::max(_rowHeight, height); ++_column; }
		_open = false;
	}
	void EndSection() {
		End(); FlushRow();
		if (_commonSide) {
			const float x = _origin.x + 260.f * _scale + _gap * .5f;
			Line(x, _commonTop, std::max(_y, _reservedBottom));
		}
		_y = std::max(_y, _reservedBottom);
		_reservedBottom = 0; _commonSide = false;
		Region(0, _width);
	}
	void Finish() {
		EndSection();
		_window->WorkRect.Max.x = _workRight;
		_window->ContentRegionRect.Max.x = _contentRight;
		ImGui::SetCursorPos(_origin);
		ImGui::Dummy({_width, _y});
	}
	ParameterPanelAnchor hovered{};

private:
	void Region(float x, float width) {
		_x = x; _regionWidth = width;
		_columns = std::max(1, int(std::floor((width + _gap) / (280.f * _scale + _gap))));
		_columnWidth = (width - (_columns - 1) * _gap) / _columns;
	}
	void FlushRow() {
		for (int column = 1; column < _column; ++column)
			Line(_origin.x + _x + column * (_columnWidth + _gap) - _gap * .5f, _y, _y + _rowHeight);
		_y += _rowHeight; _rowHeight = 0; _column = 0;
	}
	void Begin(const char* id, float x, float y, float width, bool span) {
		_id = ImGui::GetID(id); _span = span; _blockWidth = width;
		ImGui::SetCursorPos({_origin.x + x, _origin.y + y});
		_window->WorkRect.Max.x = _window->ContentRegionRect.Max.x = ImGui::GetCursorScreenPos().x + width;
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
		ImGui::BeginGroup(); _open = true;
	}
	void Line(float x, float top, float bottom) {
		x += _window->Pos.x - _window->Scroll.x;
		const float y = _window->Pos.y + _origin.y - _window->Scroll.y;
		ImGui::GetWindowDrawList()->AddLine({x, y + top}, {x, y + bottom}, ImGui::GetColorU32(ImGuiCol_Border, .45f));
	}
	ImGuiWindow* _window;
	ImVec2 _origin;
	float _width, _scale, _gap;
	ParameterPanelAnchor _restore;
	float _workRight, _contentRight;
	float _x = 0, _y = 0, _regionWidth = 0, _columnWidth = 0, _rowHeight = 0;
	float _commonTop = 0, _reservedBottom = 0, _blockWidth = 0;
	int _column = 0, _columns = 1;
	ImGuiID _id = 0;
	bool _open = false, _span = false, _commonSide = false;
};

}
