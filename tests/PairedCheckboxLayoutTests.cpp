// Native XAML measurement only: no HWND, app startup, message pump or UI automation.
#include "pch.h"
#include "EffectParameterPopupLayout.h"
#include "SimpleStackPanel.h"
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace winrt;
using namespace ::Magpie;

struct LayoutTestApp : ApplicationT<LayoutTestApp, Markup::IXamlMetadataProvider> {
	winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider{ nullptr };
	auto& Provider() {
		if (!provider) provider = winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider();
		return provider;
	}
	Markup::IXamlType GetXamlType(Interop::TypeName const& type) { return Provider().GetXamlType(type); }
	Markup::IXamlType GetXamlType(hstring const& type) { return Provider().GetXamlType(type); }
	com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return Provider().GetXmlnsDefinitions(); }
};

#include "PairedCheckboxProduction.inc"

int main() try {
	init_apartment(apartment_type::single_threaded);
	const auto app = make_self<LayoutTestApp>();
	const auto manager = Hosting::WindowsXamlManager::InitializeForCurrentThread();
	app->Provider();
	MUXC::XamlControlsResources winui;
	winui.ControlsResourcesVersion(MUXC::ControlsResourcesVersion::Version2);
	app->Resources().MergedDictionaries().Append(winui);
	unsigned cases = 0;
	// Literal-layout cases complement the compiled x:Load app probe. A hidden
	// companion and an absent companion are different lifetime states.
	for (const auto& labels : PairLabels) for (float width : {240.f,260.f,420.f}) for (int pair : {0,2}) for (int state : {0,1,2}) {
		const bool paired = state == 2;
		const auto row = Markup::XamlReader::Load(PairedRowXaml).as<Grid>();
		const auto left = row.FindName(L"Left").as<CheckBox>();
		const auto right = row.FindName(L"Right").as<CheckBox>();
		left.Content().as<TextBlock>().Text(labels[pair]);
		right.Content().as<TextBlock>().Text(labels[pair+1]);
		if (state == 0) row.Children().RemoveAt(1);
		else if (state == 1) right.Visibility(Visibility::Collapsed);
		const auto column = Markup::XamlReader::Load(ColumnShellXaml).as<Grid>();
		column.Width(width);
		const auto content = make_self<winrt::Magpie::implementation::SimpleStackPanel>();
		content->Children().Append(row); column.Children().Append(content.as<UIElement>());
		const auto columns = make_self<winrt::Magpie::implementation::SimpleStackPanel>();
		columns->Orientation(Orientation::Horizontal); columns->Spacing(24);
		columns->Children().Append(column);
		Grid nextColumn; nextColumn.Width(260); columns->Children().Append(nextColumn);
		auto measure = [&] {
			columns->Measure({std::numeric_limits<float>::infinity(),500});
			columns->Arrange({0,0,columns->DesiredSize().Width,500});
			ParameterCheckboxRow_SizeChanged(row,nullptr);
		};
		for (int i=0;i<4;++i) measure();
		const double stableWidth=column.ActualWidth(), stableMinimum=column.MinWidth();
		for (int i=0;i<8;++i) measure();
		assert(column.ActualWidth()==stableWidth && column.MinWidth()==stableMinimum);
		assert(column.MinWidth()==0 && column.ActualWidth()==width);
		if (!paired) {
			assert(column.MinWidth()==0 && column.ActualWidth()==width);
			assert(row.ColumnDefinitions().GetAt(0).Width().GridUnitType == GridUnitType::Star);
			// Reintroducing/removing the companion must also update the row.
			if (state == 0) row.Children().Append(right);
			right.Visibility(Visibility::Visible); measure(); measure();
			assert(row.ColumnDefinitions().GetAt(0).Width().GridUnitType == GridUnitType::Auto);
			if (state == 0) row.Children().RemoveAt(1);
			else right.Visibility(Visibility::Collapsed);
			measure(); measure();
			assert(row.ColumnDefinitions().GetAt(0).Width().GridUnitType == GridUnitType::Star);
			++cases; continue;
		}
		const auto rightPos = right.TransformToVisual(row).TransformPoint({0,0});
		const auto leftPos = left.TransformToVisual(row).TransformPoint({0,0});
		const auto leftEnd = leftPos.X + left.ActualWidth();
		const auto rightText = right.Content().as<TextBlock>();
		std::cout << "Pair " << cases++ << " width=" << width << " actual=" << column.ActualWidth() << " min=" << column.MinWidth() << " row=" << row.ActualWidth() << " left=" << left.ActualWidth()
			<< " rightX=" << rightPos.X << " right=" << right.ActualWidth()
			<< " rightText=" << rightText.ActualWidth() << "\n";
		assert(std::abs(rightPos.X - std::max(row.ActualWidth()/2,leftEnd+8)) < .1);
		assert(right.ActualWidth()+.1 >= rightText.ActualWidth()+28);
		assert(rightPos.X+right.ActualWidth() <= row.ActualWidth()+.1);
		assert(row.ActualWidth() <= column.ActualWidth()+.1);
		const auto nextPos=nextColumn.TransformToVisual(columns.as<UIElement>()).TransformPoint({0,0});
		assert(nextPos.X >= column.ActualWidth()+24-.1);
		assert(columns->DesiredSize().Width >= column.ActualWidth()+24+260-.1);
		column.Width(width+100); measure(); measure();
		column.Width(width); measure(); measure();
		assert(column.ActualWidth()==stableWidth && column.MinWidth()==stableMinimum);
	}
	std::cout << "PASS " << cases << " native XAML paired-checkbox measurements.\n";
} catch (const hresult_error& error) {
	std::cerr << to_string(error.message()) << "\n";
	return 1;
}
