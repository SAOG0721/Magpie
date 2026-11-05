#include "pch.h"
#include "DLSSNRDetailParameters.h"
#include "ScalingModeItem.h"
#if __has_include("ScalingModeItem.g.cpp")
#include "ScalingModeItem.g.cpp"
#endif
#include "ScalingMode.h"
#include "StrHelper.h"
#include "XamlHelper.h"
#include "AppSettings.h"
#include "EffectsService.h"
#include "EffectHelper.h"
#include "CommonSharedConstants.h"
#include "App.h"
#include "ScalingModeEffectItem.h"
#include "ScalingOptions.h"
#include "RootPage.h"

using namespace ::Magpie;

namespace winrt::Magpie::implementation {

static ScalingModeEffectItem& GetEffectItemImpl(const IInspectable& item) noexcept {
	return *get_self<ScalingModeEffectItem>(item.try_as<winrt::Magpie::ScalingModeEffectItem>());
}

ScalingModeItem::ScalingModeItem(
	uint32_t index,
	bool isInitialExpanded,
	bool shouldAutoRename)
	: _index(index),
	  _isInitialExpanded(isInitialExpanded),
	  _shouldAutoRename(shouldAutoRename)
{
	{
		std::vector<IInspectable> linkedProfiles;
		const Profile& defaultProfile = AppSettings::Get().DefaultProfile();
		if (defaultProfile.scalingMode == (int)index) {
			hstring defaults = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID)
				.GetString(L"Root_Defaults/Content");
			linkedProfiles.push_back(box_value(defaults));
		}
		for (const Profile& profile : AppSettings::Get().Profiles()) {
			if (profile.scalingMode == (int)index) {
				linkedProfiles.push_back(box_value(profile.name));
			}
		}
		_linkedProfiles = single_threaded_vector(std::move(linkedProfiles));
	}

	_scalingModeAddedRevoker = ScalingModesService::Get().ScalingModeAdded(
		auto_revoke, std::bind_front(&ScalingModeItem::_ScalingModesService_Added, this));
	_scalingModeMovedRevoker = ScalingModesService::Get().ScalingModeMoved(
		auto_revoke, std::bind_front(&ScalingModeItem::_ScalingModesService_Moved, this));
	_scalingModeNamesChangedRevoker = ScalingModesService::Get().ScalingModeNamesChanged(
		auto_revoke, std::bind_front(&ScalingModeItem::_ScalingModesService_NamesChanged, this));

	ScalingMode& data = _Data();
	{
		std::vector<IInspectable> effects;
		effects.reserve(data.effects.size());
		for (uint32_t i = 0; i < data.effects.size(); ++i) {
			effects.push_back(*_CreateScalingModeEffectItem(_index, i));
		}
		_effects = single_threaded_observable_vector(std::move(effects));
	}
	_effectsChangedRevoker = _effects.VectorChanged(
		auto_revoke, { this, &ScalingModeItem::_Effects_VectorChanged });
}

void ScalingModeItem::_Index(uint32_t value) noexcept {
	_index = value;
	for (const IInspectable& item : _effects) {
		GetEffectItemImpl(item).ScalingModeIdx(value);
	}
}

// 效果被删除后 ScalingModeItem 不会立刻析构，而且 WinUI 可能会更新绑定！我们要
// 确保被删除后 ScalingModeItem 依然处于合法的状态，调用任何方法都不会崩溃。我知
// 道老是检查显得啰嗦但别无他法。
bool ScalingModeItem::_IsRemoved() const noexcept {
	return _index == std::numeric_limits<uint32_t>::max();
}

void ScalingModeItem::_ScalingModesService_Added(EffectAddedWay) {
	RaisePropertyChanged(L"CanDrag");
}

void ScalingModeItem::_ScalingModesService_Moved(uint32_t fromIndex, uint32_t toIndex) {
	if (_index == fromIndex) {
		_Index(toIndex);
	} else if (fromIndex < toIndex && _index > fromIndex && _index <= toIndex) {
		_Index(_index - 1);
	} else if (toIndex < fromIndex && _index >= toIndex && _index < fromIndex) {
		_Index(_index + 1);
	}
}

void ScalingModeItem::PrepareForRemoval(uint32_t index) noexcept {
	if (_IsRemoved()) return;
	if (_index == index) Detach();
	else if (_index > index) _Index(_index - 1);
}

void ScalingModeItem::RefreshAfterRemoval() {
	if (_IsRemoved()) return;
	RaisePropertyChanged(L"CanDrag");
	_ScalingModesService_NamesChanged();
}

void ScalingModeItem::_ScalingModesService_NamesChanged() {
	if (_IsRemoved()) return;
	RaisePropertyChanged(L"Name");
	RaisePropertyChanged(L"HasNameConflict");
	RenameText(_renameText);
}

void ScalingModeItem::_Effects_VectorChanged(IObservableVector<IInspectable> const&, IVectorChangedEventArgs const& args) {
	if (!_isMovingEffects) {
		RaisePropertyChanged(L"Description");
		RaisePropertyChanged(L"CanReorderEffects");
		return;
	}
	
	// 移动元素时先删除再插入
	if (args.CollectionChange() == CollectionChange::ItemRemoved) {
		_movingFromIdx = args.Index();
		return;
	}
	if (args.CollectionChange() != CollectionChange::ItemInserted ||
		_movingFromIdx == std::numeric_limits<uint32_t>::max()) {
		return;
	}

	const uint32_t movingFromIdx = std::exchange(
		_movingFromIdx,
		std::numeric_limits<uint32_t>::max());
	const uint32_t movingToIdx = args.Index();

	std::vector<EffectItem>& effects = _Data().effects;
	if (movingFromIdx >= effects.size() || movingToIdx >= effects.size()) {
		return;
	}

	EffectItem removedEffect = std::move(effects[movingFromIdx]);
	effects.erase(effects.begin() + movingFromIdx);
	effects.emplace(effects.begin() + movingToIdx, std::move(removedEffect));

	uint32_t minIdx, maxIdx;
	if (movingFromIdx < movingToIdx) {
		minIdx = movingFromIdx;
		maxIdx = movingToIdx;
	} else {
		minIdx = movingToIdx;
		maxIdx = movingFromIdx;
	}
	
	for (uint32_t i = minIdx; i <= maxIdx; ++i) {
		GetEffectItemImpl(_effects.GetAt(i)).EffectIdx(i);
	}

	RaisePropertyChanged(L"Description");
	AppSettings::Get().SaveAsync();
}

void ScalingModeItem::_ScalingModeEffectItem_Removed(uint32_t index) {
	if (_IsRemoved()) {
		return;
	}

	std::vector<EffectItem>& effects = _Data().effects;
	effects.erase(effects.begin() + index);

	_isMovingEffects = false;
	// 标记已被删除
	GetEffectItemImpl(_effects.GetAt(index)).EffectIdx(std::numeric_limits<uint32_t>::max());
	_effects.RemoveAt(index);
	_isMovingEffects = true;

	for (uint32_t i = index; i < effects.size(); ++i) {
		GetEffectItemImpl(_effects.GetAt(i)).EffectIdx(i);
	}

	_RefreshEffectDragState();

	RaisePropertyChanged(L"HasUnkownEffects");

	AppSettings::Get().SaveAsync();
}

com_ptr<ScalingModeEffectItem> ScalingModeItem::_CreateScalingModeEffectItem(uint32_t scalingModeIdx, uint32_t effectIdx) {
	auto item = make_self<ScalingModeEffectItem>(scalingModeIdx, effectIdx);
	item->Removed(std::bind_front(&ScalingModeItem::_ScalingModeEffectItem_Removed, this));
	return item;
}

void ScalingModeItem::_RefreshEffectDragState() {
	for (const IInspectable& item : _effects) {
		GetEffectItemImpl(item).RefreshDragState();
	}
}

void ScalingModeItem::AddEffect(const hstring& fullName) {
	if (!CanAddEffect(fullName)) {
		return;
	}

	const EffectInfo* effectInfo = EffectsService::Get().GetEffect(fullName);
	if (!effectInfo) return;
	EffectItem& effect = _Data().effects.emplace_back();
	effect.name = fullName;
	NormalizeFrameGenerationParameters(effect);
	if (fullName == L"DLSSNR\\DLSSNR_AI_Filter") {
		effect.parameters[L"resolutionControl"] = 0.f;
		effect.parameters[L"__dlssnrModeVersion"] = DLSSNR_MODE_VERSION;
		NormalizeDLSSNRDetailParameters(effect.parameters);
	}
	if (effectInfo->CanScale()) {
		// 支持缩放的效果默认等比缩放到充满屏幕
		effect.scalingType = ::Magpie::ScalingType::Fit;
	}

	auto item = _CreateScalingModeEffectItem(_index, (uint32_t)_Data().effects.size() - 1);
	_isMovingEffects = false;
	_effects.Append(*item);
	_isMovingEffects = true;

	_RefreshEffectDragState();

	AppSettings::Get().SaveAsync();
}

bool ScalingModeItem::CanAddEffect(const hstring& fullName) const noexcept {
	if (_IsRemoved() || _Data().hdrMigrationInvalid || !EffectsService::Get().GetEffect(fullName)) {
		return false;
	}

	const FrameGenerationEffectKind candidate =
		ClassifyFrameGenerationEffect(std::wstring_view(fullName));
	if (candidate == FrameGenerationEffectKind::None) {
		return true;
	}

	return !ValidateFrameGenerationChain(_Data().effects).HasFrameGeneration();
}

hstring ScalingModeItem::EffectAddProblem(const hstring& fullName) const {
	if (_IsRemoved()) return L"请重新打开要编辑的效果组，再添加效果器。";
	if (_Data().hdrMigrationInvalid) return ResourceLoader::GetForCurrentView(
		CommonSharedConstants::APP_RESOURCE_MAP_ID).GetString(L"ScalingModes_HdrMigrationInvalid_Description");
	if (!EffectsService::Get().GetEffect(fullName)) return L"请恢复对应效果器文件并重启 Magpie，再尝试添加。";
	if (CanAddEffect(fullName)) return {};
	for (const auto& effect : _Data().effects) {
		if (ClassifyFrameGenerationEffect(effect.name) != FrameGenerationEffectKind::None) {
			return hstring(fmt::format(L"当前组已使用 {}；如需改用 {}，请先移除现有补帧项。",
				EffectHelper::GetDisplayName(effect.name), EffectHelper::GetDisplayName(fullName)));
		}
	}
	return L"请重新打开当前效果组后再添加。";
}

hstring ScalingModeItem::Name() const noexcept {
	if (_IsRemoved()) {
		return {};
	}

	return hstring(_Data().name);
}

void ScalingModeItem::Name(const hstring& value) noexcept {
	if (_IsRemoved()) {
		return;
	}

	if (!ScalingModesService::Get().RenameScalingMode(_index, value)) RenameText(value);
}

hstring ScalingModeItem::Description() const noexcept {
	if (_IsRemoved()) {
		return {};
	}
	if (_Data().hdrMigrationInvalid) return ResourceLoader::GetForCurrentView(
		CommonSharedConstants::APP_RESOURCE_MAP_ID).GetString(L"ScalingModes_HdrMigrationInvalid_Description");

	std::wstring result;
	for (const EffectItem& effect : _Data().effects) {
		if (!result.empty()) {
			result.append(L" > ");
		}

		if (EffectsService::Get().GetEffect(effect.name) != nullptr) {
			result += EffectHelper::GetDisplayName(effect.name);
		} else {
			ResourceLoader resourceLoader =
				ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
			result += L'(';
			result += resourceLoader.GetString(L"ScalingModes_Description_UnknownEffect");
			result += L')';
		}
	}
	return hstring(result);
}

bool ScalingModeItem::HasUnkownEffects() const noexcept {
	if (_IsRemoved()) {
		return false;
	}

	for (const EffectItem& effect : _Data().effects) {
		if (!EffectsService::Get().GetEffect(effect.name)) {
			return true;
		}
	}

	return false;
}

bool ScalingModeItem::HasNameConflict() const noexcept {
	return !_IsRemoved() && ScalingModesService::Get().HasNameConflict(_index);
}

void ScalingModeItem::RenameText(const hstring& value) noexcept {
	if (_IsRemoved()) {
		return;
	}

	_renameText = value;
	RaisePropertyChanged(L"RenameText");

	_trimedRenameText = value;
	StrHelper::Trim(_trimedRenameText);
	const auto loader = ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	if (_trimedRenameText.empty()) _renameProblem = loader.GetString(L"ScalingModes_NameRequired");
	else if (!ScalingModesService::Get().CanUseName(_trimedRenameText, _index))
		_renameProblem = loader.GetString(L"ScalingModes_NameAlreadyExists");
	else _renameProblem = {};
	RaisePropertyChanged(L"RenameProblem");
	RaisePropertyChanged(L"HasRenameProblem");
	bool newEnabled = _renameProblem.empty() && _trimedRenameText != _Data().name;
	if (_isRenameButtonEnabled != newEnabled) {
		_isRenameButtonEnabled = newEnabled;
		RaisePropertyChanged(L"IsRenameButtonEnabled");
	}
}

void ScalingModeItem::RenameFlyout_Opening() {
	if (_IsRemoved()) {
		return;
	}

	RenameText(hstring(_Data().name));
	RaisePropertyChanged(L"RenameTextBoxSelectionStart");
}

void ScalingModeItem::RenameTextBox_KeyDown(IInspectable const&, Input::KeyRoutedEventArgs const& args) {
	if (_IsRemoved()) {
		return;
	}

	if (args.Key() == VirtualKey::Enter) {
		RenameButton_Click();
	}
}

void ScalingModeItem::RenameButton_Click() {
	if (_IsRemoved() || !_isRenameButtonEnabled) {
		return;
	}
	auto lifetime = get_strong();
	if (!ScalingModesService::Get().RenameScalingMode(_index, _trimedRenameText)) {
		RenameText(_renameText);
		return;
	}

	// Flyout 没有 IsOpen 可供绑定，只能用变通方法关闭
	XamlHelper::ClosePopups(App::Get().RootPage()->XamlRoot());

}

bool ScalingModeItem::TakeAutoRenameRequest() noexcept {
	return std::exchange(_shouldAutoRename, false);
}

bool ScalingModeItem::CanDrag() const noexcept {
	return !_IsRemoved() &&
		ScalingModesService::Get().GetScalingModeCount() > 1;
}

void ScalingModeItem::Duplicate() {
	if (_IsRemoved()) {
		return;
	}

	ResourceLoader resourceLoader =
		ResourceLoader::GetForCurrentView(CommonSharedConstants::APP_RESOURCE_MAP_ID);
	std::wstring name = _Data().name;
	name += resourceLoader.GetString(L"ScalingModes_DuplicateSuffix");
	ScalingModesService::Get().AddScalingMode(name, (int)_index);
}

bool ScalingModeItem::CanReorderEffects() const noexcept {
	if (_IsRemoved()) {
		return false;
	}

	return !_Data().hdrMigrationInvalid && _effects.Size() > 1;
}

void ScalingModeItem::Remove() {
	if (_IsRemoved()) {
		return;
	}
	auto lifetime = get_strong();
	const uint32_t index = _index;
	// RemoveAt can synchronously re-enter bindings and release the last UI reference.
	Detach();
	ScalingModesService::Get().RemoveScalingMode(index);
}

void ScalingModeItem::Detach() noexcept {
	if (_IsRemoved()) return;
	_Index(std::numeric_limits<uint32_t>::max());

	// 被删除后不会立刻析构，因此手动清理事件订阅
	_effectsChangedRevoker.revoke();
	_scalingModeAddedRevoker.Revoke();
	_scalingModeMovedRevoker.Revoke();
	_scalingModeNamesChangedRevoker.Revoke();
}

ScalingMode& ScalingModeItem::_Data() noexcept {
	return ScalingModesService::Get().GetScalingMode(_index);
}

const ScalingMode& ScalingModeItem::_Data() const noexcept {
	return ScalingModesService::Get().GetScalingMode(_index);
}

}
