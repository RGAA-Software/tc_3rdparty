//
// Created by RGAA on 26/03/2025.
//

#ifndef GAMMARAY_EXPECTED_H
#define GAMMARAY_EXPECTED_H

#include <expected>
#include <string>

namespace tc
{

    template<class T, class E>
    using Result = std::expected<T, E>;

    template<typename T>
    using ResultStrErr = Result<T, std::string>;

    template<class T>
    static Result<T, std::string> Err(const std::string& err) {
        return std::unexpected(err);
    }

    template<class T>
    static Result<T, int> ErrInt(int err) {
        return std::unexpected(err);
    }


#define TRError std::unexpected
#define TcErr std::unexpected

}

#endif //GAMMARAY_EXPECTED_H
