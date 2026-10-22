// Compiled after the production functions by prepare_screenshot_save_tests.py.
static std::string ReadFile(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), {} };
}

static void DecodeSize(const std::filesystem::path& path, uint32_t width, uint32_t height) {
	winrt::com_ptr<IWICImagingFactory2> factory;
	assert(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))));
	winrt::com_ptr<IWICBitmapDecoder> decoder;
	assert(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.put())));
	winrt::com_ptr<IWICBitmapFrameDecode> frame;
	assert(SUCCEEDED(decoder->GetFrame(0, frame.put())));
	UINT actualWidth = 0, actualHeight = 0;
	assert(SUCCEEDED(frame->GetSize(&actualWidth, &actualHeight)));
	assert(actualWidth == width && actualHeight == height);
}

int wmain(int argc, wchar_t** argv) {
	assert(argc == 2);
	using namespace Magpie;
	winrt::init_apartment(winrt::apartment_type::multi_threaded);
	const auto root = std::filesystem::path(argv[1]) / (L"disk-" + std::to_wstring(GetCurrentProcessId()));
	std::filesystem::create_directories(root);
	const auto cleanup = wil::scope_exit([&] { std::filesystem::remove_all(root); });
	std::vector<uint8_t> pixels(26 * 14 * 4, 255);
	auto save = [&](const std::filesystem::path& path, uint32_t w = 13, uint32_t h = 7) {
		TextureSaveError error;
		return TextureHelper::SaveTextureExclusive(path.c_str(), w, h,
			DXGI_FORMAT_R8G8B8A8_UNORM, pixels, w * 4, &error);
	};
	// Hold the request timestamp fixed while later writes and all workers run.
	const std::wstring timestamp = L"20261022_194530_123";
	const auto captured = ScreenshotHelper::CaptureTimestamp();
	assert(captured.size() == 19 && captured[8] == L'_' && captured[15] == L'_');
	for (size_t i = 0; i < captured.size(); ++i)
		if (i != 8 && i != 15) assert(captured[i] >= L'0' && captured[i] <= L'9');
	std::ofstream(root/L"Magpie_001.png") << "legacy screenshot";
	std::ofstream(root/L"Magpie_4294967295_input.png") << "legacy pair";
	const auto base = ScreenshotHelper::FindUnusedScreenshotStem(root, timestamp);
	assert(base == L"Magpie_20261022_194530_123");
	assert(save(root/(base + L"_input.png")));
	assert(save(root/(base + L"_output.png"), 26, 14));
	DecodeSize(root/(base + L"_input.png"), 13, 7);
	DecodeSize(root/(base + L"_output.png"), 26, 14);
	const auto original = ReadFile(root/(base + L"_input.png"));
	assert(!original.empty());
	assert(!save(root/(base + L"_input.png")));
	assert(ReadFile(root/(base + L"_input.png")) == original);
	const auto partial = ScreenshotHelper::FindUnusedScreenshotStem(root, timestamp);
	assert(partial == base + L"_01");
	// Simulate a path becoming occupied between naming and exclusive commit.
	std::filesystem::create_directory(root/(partial + L"_output.png"));
	assert(save(root/(partial + L"_input.png")));
	assert(!save(root/(partial + L"_output.png")));
	assert(std::filesystem::is_regular_file(root/(partial + L"_input.png")));
	const auto single = ScreenshotHelper::FindUnusedScreenshotStem(root, timestamp);
	assert(single == base + L"_02");
	assert(save(root/(single + L"_output.png")));
	const auto exported = ScreenshotHelper::FindUnusedScreenshotStem(root, timestamp);
	assert(exported == base + L"_03");
	std::ofstream(root/(exported + L"_effect_01_pass_02_texture_03.dds")) << "intermediate";
	std::vector<std::thread> workers;
	for (int i = 0; i < 8; ++i) workers.emplace_back([&] {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		std::lock_guard lock(ScreenshotHelper::SaveMutex());
		const auto stem = ScreenshotHelper::FindUnusedScreenshotStem(root, timestamp);
		assert(save(root/(stem + L"_input.png")));
		assert(save(root/(stem + L"_output.png"), 26, 14));
	});
	for (auto& worker : workers) worker.join();
	assert(ScreenshotHelper::FindUnusedScreenshotStem(root, timestamp) == base + L"_12");
	for (uint32_t i = 4; i < 12; ++i) {
		DecodeSize(root/(ScreenshotStem(timestamp, i) + L"_input.png"), 13, 7);
		DecodeSize(root/(ScreenshotStem(timestamp, i) + L"_output.png"), 26, 14);
	}
	assert(ScreenshotHelper::FindUnusedScreenshotStem(root, L"20261022_194530_124") == L"Magpie_20261022_194530_124");
	std::ofstream(root/L"MAGPIE_20261022_194530_124_INPUT.PNG") << "case-insensitive existing name";
	assert(ScreenshotHelper::FindUnusedScreenshotStem(root, L"20261022_194530_124") == L"Magpie_20261022_194530_124_01");
	assert(ReadFile(root/L"Magpie_001.png") == "legacy screenshot");
	for (const auto& entry : std::filesystem::directory_iterator(root))
		assert(!entry.path().filename().native().starts_with(L".Magpie-"));
	TextureSaveError error;
	const auto missing = root/L"missing"/L"Magpie_001.png";
	assert(!TextureHelper::SaveTextureExclusive(missing.c_str(), 1, 1,
		DXGI_FORMAT_R8G8B8A8_UNORM, pixels, 4, &error));
	assert(error.fileWriteFailed);
	std::cout << "PASS: production WIC PNG sizes, request timestamps, retained side/export suffixes, legacy coexistence, eight same-millisecond pairs, partial failure, no overwrite and temp cleanup\n";
}
