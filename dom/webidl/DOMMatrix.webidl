/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://drafts.fxtf.org/geometry/
 *
 * Copyright © 2012 W3C® (MIT, ERCIM, Keio), All Rights Reserved. W3C
 * liability, trademark and document use rules apply.
 */

[Constructor(optional (DOMString or sequence<unrestricted double>) init),
 Exposed=(Window,Worker)]
interface DOMMatrixReadOnly {
    [NewObject, Throws] static DOMMatrixReadOnly fromMatrix(optional DOMMatrixInit other);
    [NewObject, Throws] static DOMMatrixReadOnly fromFloat32Array(Float32Array array32);
    [NewObject, Throws] static DOMMatrixReadOnly fromFloat64Array(Float64Array array64);

    // These attributes are simple aliases for certain elements of the 4x4 matrix
    readonly attribute unrestricted double a;
    readonly attribute unrestricted double b;
    readonly attribute unrestricted double c;
    readonly attribute unrestricted double d;
    readonly attribute unrestricted double e;
    readonly attribute unrestricted double f;

    readonly attribute unrestricted double m11;
    readonly attribute unrestricted double m12;
    readonly attribute unrestricted double m13;
    readonly attribute unrestricted double m14;
    readonly attribute unrestricted double m21;
    readonly attribute unrestricted double m22;
    readonly attribute unrestricted double m23;
    readonly attribute unrestricted double m24;
    readonly attribute unrestricted double m31;
    readonly attribute unrestricted double m32;
    readonly attribute unrestricted double m33;
    readonly attribute unrestricted double m34;
    readonly attribute unrestricted double m41;
    readonly attribute unrestricted double m42;
    readonly attribute unrestricted double m43;
    readonly attribute unrestricted double m44;

    // Immutable transform methods
    DOMMatrix translate(unrestricted double tx,
                        unrestricted double ty,
                        optional unrestricted double tz = 0);
    DOMMatrix scale(unrestricted double scale,
                    optional unrestricted double originX = 0,
                    optional unrestricted double originY = 0);
    DOMMatrix scale3d(unrestricted double scale,
                      optional unrestricted double originX = 0,
                      optional unrestricted double originY = 0,
                      optional unrestricted double originZ = 0);
    DOMMatrix scaleNonUniform(unrestricted double scaleX,
                              optional unrestricted double scaleY = 1,
                              optional unrestricted double scaleZ = 1,
                              optional unrestricted double originX = 0,
                              optional unrestricted double originY = 0,
                              optional unrestricted double originZ = 0);
    [NewObject] DOMMatrix rotate(optional unrestricted double rotX = 0,
                                 optional unrestricted double rotY,
                                 optional unrestricted double rotZ);
    [NewObject] DOMMatrix rotateFromVector(optional unrestricted double x = 0,
                                           optional unrestricted double y = 0);
    [NewObject] DOMMatrix rotateAxisAngle(optional unrestricted double x = 0,
                                          optional unrestricted double y = 0,
                                          optional unrestricted double z = 0,
                                          optional unrestricted double angle = 0);
    DOMMatrix skewX(unrestricted double sx);
    DOMMatrix skewY(unrestricted double sy);
    [NewObject, Throws] DOMMatrix multiply(optional DOMMatrixInit other);
    DOMMatrix flipX();
    DOMMatrix flipY();
    DOMMatrix inverse();

    // Helper methods
    readonly attribute boolean is2D;
    readonly attribute boolean identity;
    DOMPoint                   transformPoint(optional DOMPointInit point);
    [Throws] Float32Array      toFloat32Array();
    [Throws] Float64Array      toFloat64Array();
    [Exposed=Window]           stringifier;
                               jsonifier;
};

[Constructor,
 Constructor(DOMString transformList),
 Constructor(DOMMatrixReadOnly other),
 Constructor(Float32Array array32),
 Constructor(Float64Array array64),
 Constructor(sequence<unrestricted double> numberSequence),
 Exposed=(Window,Worker)]
interface DOMMatrix : DOMMatrixReadOnly {
    [NewObject, Throws] static DOMMatrix fromMatrix(optional DOMMatrixInit other);
    [NewObject, Throws] static DOMMatrix fromFloat32Array(Float32Array array32);
    [NewObject, Throws] static DOMMatrix fromFloat64Array(Float64Array array64);

    // These attributes are simple aliases for certain elements of the 4x4 matrix
    inherit attribute unrestricted double a;
    inherit attribute unrestricted double b;
    inherit attribute unrestricted double c;
    inherit attribute unrestricted double d;
    inherit attribute unrestricted double e;
    inherit attribute unrestricted double f;

    inherit attribute unrestricted double m11;
    inherit attribute unrestricted double m12;
    inherit attribute unrestricted double m13;
    inherit attribute unrestricted double m14;
    inherit attribute unrestricted double m21;
    inherit attribute unrestricted double m22;
    inherit attribute unrestricted double m23;
    inherit attribute unrestricted double m24;
    inherit attribute unrestricted double m31;
    inherit attribute unrestricted double m32;
    inherit attribute unrestricted double m33;
    inherit attribute unrestricted double m34;
    inherit attribute unrestricted double m41;
    inherit attribute unrestricted double m42;
    inherit attribute unrestricted double m43;
    inherit attribute unrestricted double m44;

    // Mutable transform methods
    [Throws] DOMMatrix multiplySelf(optional DOMMatrixInit other);
    [Throws] DOMMatrix preMultiplySelf(optional DOMMatrixInit other);
    DOMMatrix translateSelf(unrestricted double tx,
                            unrestricted double ty,
                            optional unrestricted double tz = 0);
    DOMMatrix scaleSelf(unrestricted double scale,
                        optional unrestricted double originX = 0,
                        optional unrestricted double originY = 0);
    DOMMatrix scale3dSelf(unrestricted double scale,
                          optional unrestricted double originX = 0,
                          optional unrestricted double originY = 0,
                          optional unrestricted double originZ = 0);
    DOMMatrix scaleNonUniformSelf(unrestricted double scaleX,
                                  optional unrestricted double scaleY = 1,
                                  optional unrestricted double scaleZ = 1,
                                  optional unrestricted double originX = 0,
                                  optional unrestricted double originY = 0,
                                  optional unrestricted double originZ = 0);
    DOMMatrix rotateSelf(optional unrestricted double rotX = 0,
                         optional unrestricted double rotY,
                         optional unrestricted double rotZ);
    DOMMatrix rotateFromVectorSelf(optional unrestricted double x = 0,
                                   optional unrestricted double y = 0);
    DOMMatrix rotateAxisAngleSelf(optional unrestricted double x = 0,
                                  optional unrestricted double y = 0,
                                  optional unrestricted double z = 0,
                                  optional unrestricted double angle = 0);
    DOMMatrix skewXSelf(unrestricted double sx);
    DOMMatrix skewYSelf(unrestricted double sy);
    DOMMatrix invertSelf();
    [Exposed=Window, Throws] DOMMatrix setMatrixValue(DOMString transformList);
};

dictionary DOMMatrix2DInit {
    unrestricted double a;
    unrestricted double b;
    unrestricted double c;
    unrestricted double d;
    unrestricted double e;
    unrestricted double f;
    unrestricted double m11;
    unrestricted double m12;
    unrestricted double m21;
    unrestricted double m22;
    unrestricted double m41;
    unrestricted double m42;
};

dictionary DOMMatrixInit : DOMMatrix2DInit {
    unrestricted double m13 = 0;
    unrestricted double m14 = 0;
    unrestricted double m23 = 0;
    unrestricted double m24 = 0;
    unrestricted double m31 = 0;
    unrestricted double m32 = 0;
    unrestricted double m33 = 1;
    unrestricted double m34 = 0;
    unrestricted double m43 = 0;
    unrestricted double m44 = 1;
    boolean is2D;
};
