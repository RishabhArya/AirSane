/*
AirSane Imaging Daemon
Copyright (C) 2018 Simul Piscator

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef PNGENCODER_H
#define PNGENCODER_H

#include "imageencoder.h"

class PngEncoder : public ImageEncoder
{
public:
    PngEncoder();
    ~PngEncoder();
    PngEncoder(const PngEncoder&) = delete;
    PngEncoder& operator=(const PngEncoder&) = delete;

protected:
    void onImageBegin() override;
    void onImageEnd() override;
    void onWriteLine(const void*) override;

private:
    struct Private;
    Private* p;

};

#endif // PNGENCODER_H