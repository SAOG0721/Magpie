#pragma once

#include "HdrProtocol.h"

namespace Magpie {

HdrFormatRoutes GetGroupBHdrRoutes(
	std::string_view effectGroup,
	bool experimentalDlssnr = false
) noexcept;

}
