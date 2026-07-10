#!/usr/bin/env python3
"""
PTP Uncertainty Visualizer

Read CSV logs from watch_uncertainty -raw and plot uncertainty metrics,
synchronization status, and port state over time.
"""

import argparse
import os
import sys

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import pandas as pd

PORT_STATE_NAMES = {
    0: "UNKNOWN",
    1: "INITIALIZING",
    2: "FAULTY",
    3: "DISABLED",
    4: "LISTENING",
    5: "PRE_MASTER",
    6: "MASTER",
    7: "PASSIVE",
    8: "UNCALIBRATED",
    9: "SLAVE",
}

QUALITY_COLORS = {
    "EXCELLENT": "#06A77D",
    "GOOD": "#90BE6D",
    "MODERATE": "#F9C74F",
    "POOR": "#F94144",
    "N/A": "#CCCCCC",
}


def load_csv_data(filepath):
    """Load and parse a watch_uncertainty CSV log."""
    try:
        df = pd.read_csv(filepath)
    except FileNotFoundError:
        print(f"ERROR: File not found: {filepath}")
        sys.exit(1)
    except Exception as exc:
        print(f"ERROR: Failed to load CSV file: {exc}")
        sys.exit(1)

    required = {
        "timestamp_ns",
        "ptp4l_connected",
        "is_synchronized",
        "port_state",
        "offset_from_master_ns",
        "mean_path_delay_ns",
        "drift_ns",
        "total_uncertainty_ns",
    }
    missing = required - set(df.columns)
    if missing:
        print(f"ERROR: CSV is missing required columns: {', '.join(sorted(missing))}")
        sys.exit(1)

    df["timestamp"] = pd.to_datetime(df["timestamp_ns"], unit="ns")
    df["port_status"] = df["port_state"].map(PORT_STATE_NAMES).fillna("UNKNOWN")
    df["sync_state"] = df.apply(derive_sync_state, axis=1)
    df["quality"] = df["total_uncertainty_ns"].apply(classify_quality)
    return df


def derive_sync_state(row):
    if row["ptp4l_connected"] == 0:
        return "PTP4L_NOT_CONNECTED"
    if row["is_synchronized"] == 0:
        return "NOT_SYNCHRONIZED"
    return "SYNCHRONIZED"


def classify_quality(total_uncertainty_ns):
    total_us = total_uncertainty_ns / 1000.0
    if total_us < 1.0:
        return "EXCELLENT"
    if total_us < 10.0:
        return "GOOD"
    if total_us < 100.0:
        return "MODERATE"
    return "POOR"


def format_time_axis(ax):
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))
    plt.setp(ax.xaxis.get_majorticklabels(), rotation=45, ha="right")


def plot_uncertainty_components(df, ax):
    """Plot total uncertainty and its components over time."""
    ax.plot(
        df["timestamp"],
        df["total_uncertainty_ns"] / 1000.0,
        label="Total Uncertainty",
        color="#2E86AB",
        linewidth=1.5,
    )
    ax.plot(
        df["timestamp"],
        df["offset_from_master_ns"] / 1000.0,
        label="|Offset from Master|",
        color="#F18F01",
        linewidth=1.2,
        alpha=0.8,
    )
    ax.plot(
        df["timestamp"],
        df["mean_path_delay_ns"].abs() / 1000.0,
        label="|Mean Path Delay|",
        color="#A23B72",
        linewidth=1.2,
        alpha=0.8,
    )
    ax.plot(
        df["timestamp"],
        df["drift_ns"] / 1000.0,
        label="Drift",
        color="#06A77D",
        linewidth=1.2,
        alpha=0.8,
    )

    ax.set_ylabel("Uncertainty (μs)", fontsize=11, fontweight="bold")
    ax.set_title("PTP Uncertainty Over Time", fontsize=13, fontweight="bold", pad=15)
    ax.legend(loc="upper right", framealpha=0.9)
    ax.grid(True, alpha=0.3, linestyle="--")
    ax.set_yscale("log")
    format_time_axis(ax)


def plot_offset_from_master(df, ax):
    """Plot offset magnitude over time."""
    ax.plot(
        df["timestamp"],
        df["offset_from_master_ns"] / 1000.0,
        color="#06A77D",
        linewidth=1.2,
    )
    ax.set_ylabel("Offset (μs)", fontsize=11, fontweight="bold")
    ax.set_title("Offset from Master Clock", fontsize=13, fontweight="bold", pad=15)
    ax.grid(True, alpha=0.3, linestyle="--")
    format_time_axis(ax)


def plot_quality_distribution(df, ax):
    """Plot uncertainty quality distribution."""
    quality_counts = df["quality"].value_counts()
    plot_colors = [QUALITY_COLORS.get(q, "#CCCCCC") for q in quality_counts.index]

    ax.pie(
        quality_counts.values,
        labels=quality_counts.index,
        autopct="%1.1f%%",
        colors=plot_colors,
        startangle=90,
        textprops={"fontsize": 10, "fontweight": "bold"},
    )
    ax.set_title("Uncertainty Quality Distribution", fontsize=13, fontweight="bold", pad=15)


def plot_sync_state(df, ax):
    """Plot synchronization state over time."""
    state_map = {
        "PTP4L_NOT_CONNECTED": 0,
        "NOT_SYNCHRONIZED": 1,
        "SYNCHRONIZED": 2,
    }

    sync_numeric = df["sync_state"].map(state_map)
    ax.step(df["timestamp"], sync_numeric, where="post", linewidth=2, color="#2E86AB")
    ax.set_ylabel("Sync State", fontsize=11, fontweight="bold")
    ax.set_title("PTP Synchronization State", fontsize=13, fontweight="bold", pad=15)
    ax.set_yticks([0, 1, 2])
    ax.set_yticklabels(["PTP4L\nNOT CONNECTED", "NOT\nSYNCHRONIZED", "SYNCHRONIZED"])
    ax.set_ylim(-0.5, 2.5)
    ax.grid(True, alpha=0.3, linestyle="--", axis="x")
    ax.axhspan(-0.5, 0.5, facecolor="red", alpha=0.1)
    ax.axhspan(0.5, 1.5, facecolor="yellow", alpha=0.1)
    ax.axhspan(1.5, 2.5, facecolor="green", alpha=0.1)
    format_time_axis(ax)


def plot_port_state_timeline(df, ax):
    """Plot IEEE 1588 port state over time."""
    ax.step(
        df["timestamp"],
        df["port_state"],
        where="post",
        linewidth=2,
        color="#A23B72",
    )

    unique_values = sorted(df["port_state"].unique())
    unique_labels = [PORT_STATE_NAMES.get(v, "UNKNOWN") for v in unique_values]

    ax.set_ylabel("Port State", fontsize=11, fontweight="bold")
    ax.set_xlabel("Time", fontsize=11, fontweight="bold")
    ax.set_title("PTP Port State Timeline", fontsize=13, fontweight="bold", pad=15)
    ax.set_yticks(unique_values)
    ax.set_yticklabels(unique_labels)
    ax.grid(True, alpha=0.3, linestyle="--", axis="x")
    format_time_axis(ax)


def print_summary_statistics(df):
    """Print summary statistics to the console."""
    print("\n" + "=" * 60)
    print("PTP UNCERTAINTY SUMMARY STATISTICS")
    print("=" * 60)

    print("\nTime Range:")
    print(f"  Start: {df['timestamp'].min()}")
    print(f"  End:   {df['timestamp'].max()}")
    print(f"  Duration: {df['timestamp'].max() - df['timestamp'].min()}")
    print(f"  Total Samples: {len(df)}")

    print("\nSynchronization State:")
    for state, count in df["sync_state"].value_counts().items():
        percentage = (count / len(df)) * 100
        print(f"  {state}: {count} samples ({percentage:.1f}%)")

    print("\nQuality Distribution:")
    for quality, count in df["quality"].value_counts().items():
        percentage = (count / len(df)) * 100
        print(f"  {quality}: {count} samples ({percentage:.1f}%)")

    sync_df = df[df["is_synchronized"] == 1]
    if len(sync_df) > 0:
        print(f"\nUncertainty (Synchronized samples only, n={len(sync_df)}):")
        for label, column in (
            ("Total Uncertainty", "total_uncertainty_ns"),
            ("Offset from Master", "offset_from_master_ns"),
            ("Mean Path Delay", "mean_path_delay_ns"),
            ("Drift", "drift_ns"),
        ):
            series = sync_df[column].abs() if column == "mean_path_delay_ns" else sync_df[column]
            print(f"  {label}:")
            print(f"    Mean:   {series.mean() / 1000.0:.2f} μs")
            print(f"    Median: {series.median() / 1000.0:.2f} μs")
            print(f"    Min:    {series.min() / 1000.0:.2f} μs")
            print(f"    Max:    {series.max() / 1000.0:.2f} μs")
            print(f"    Std:    {series.std() / 1000.0:.2f} μs")

        if "snapshot_age_ms" in sync_df.columns:
            print("\n  Snapshot Age:")
            print(f"    Mean:   {sync_df['snapshot_age_ms'].mean():.2f} ms")
            print(f"    Median: {sync_df['snapshot_age_ms'].median():.2f} ms")
            print(f"    Max:    {sync_df['snapshot_age_ms'].max():.2f} ms")

    if "gm_id" in df.columns:
        gm_ids = df.loc[df["gm_id"].astype(str).str.len() > 0, "gm_id"].unique()
        print("\nGrandmaster Information:")
        print(f"  Unique Grandmasters: {len(gm_ids)}")
        for gm in gm_ids:
            count = (df["gm_id"] == gm).sum()
            percentage = (count / len(df)) * 100
            print(f"    {gm}: {count} samples ({percentage:.1f}%)")

    print("\n" + "=" * 60 + "\n")


def create_visualization(csv_file, output_file=None, show_plot=True):
    """Create a visualization dashboard from a CSV log."""
    print(f"Loading data from: {csv_file}")
    df = load_csv_data(csv_file)

    print(f"Loaded {len(df)} samples")
    print_summary_statistics(df)

    fig = plt.figure(figsize=(16, 12))
    gs = fig.add_gridspec(
        3,
        2,
        hspace=0.35,
        wspace=0.25,
        left=0.08,
        right=0.95,
        top=0.94,
        bottom=0.06,
    )

    ax1 = fig.add_subplot(gs[0, :])
    ax2 = fig.add_subplot(gs[1, 0])
    ax3 = fig.add_subplot(gs[1, 1])
    ax4 = fig.add_subplot(gs[2, 0])
    ax5 = fig.add_subplot(gs[2, 1])

    plot_uncertainty_components(df, ax1)
    plot_offset_from_master(df, ax2)
    plot_quality_distribution(df, ax3)
    plot_sync_state(df, ax4)
    plot_port_state_timeline(df, ax5)

    fig.suptitle(
        f"PTP Uncertainty Analysis\n{os.path.basename(csv_file)}",
        fontsize=16,
        fontweight="bold",
        y=0.98,
    )

    if output_file:
        print(f"Saving visualization to: {output_file}")
        plt.savefig(output_file, dpi=150, bbox_inches="tight")
        print("Visualization saved successfully!")

    if show_plot:
        print("Displaying visualization...")
        plt.show()

    plt.close()


def main():
    parser = argparse.ArgumentParser(
        description="Visualize PTP uncertainty logs from watch_uncertainty -raw",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s uncertainty.csv
  %(prog)s uncertainty.csv --output uncertainty.png
  %(prog)s uncertainty.csv --output uncertainty.png --no-show
        """,
    )

    parser.add_argument(
        "csv_file",
        help="CSV log file generated by watch_uncertainty -raw",
    )
    parser.add_argument(
        "-o",
        "--output",
        help="Output file for saving the visualization (PNG, PDF, SVG, etc.)",
    )
    parser.add_argument(
        "--no-show",
        action="store_true",
        help="Do not display the plot (useful when only saving to file)",
    )

    args = parser.parse_args()

    if not os.path.exists(args.csv_file):
        print(f"ERROR: File not found: {args.csv_file}")
        sys.exit(1)

    create_visualization(
        args.csv_file,
        output_file=args.output,
        show_plot=not args.no_show,
    )


if __name__ == "__main__":
    main()
