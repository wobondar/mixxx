#pragma once

/// When the scrolling waveform of a deck with separated stems draws them
/// instead of the signal.
enum class LiveStemView {
    Off = 0,
    Ready = 1,        // as soon as a region is separated
    WhenAdjusted = 2, // while any stem is muted or its gain is off unity
};

/// What columns whose region is not separated yet show in the stem view.
enum class LiveStemUnseparated {
    KeepSignal = 0,
    Blank = 1,
    DimSignal = 2,
};
