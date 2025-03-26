//
// Created by RGAA on 26/03/2025.
//

#ifndef GAMMARAY_EXPECTED_H
#define GAMMARAY_EXPECTED_H

#include "expected.hpp"

namespace tc
{

    template<class T, class E>
    using Result = tl::expected<T, E>;

    template<class T>
    static Result<T, std::string> Err(const std::string& err) {
        return tl::make_unexpected(err);
    }

    #define TRError tl::make_unexpected

}

#endif //GAMMARAY_EXPECTED_H
