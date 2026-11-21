package com.wzmwayne.oledpanel

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.RecyclerView
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class LogAdapter : RecyclerView.Adapter<LogAdapter.VH>() {

    private val items = ArrayList<LogStore.Entry>()
    private val expanded = HashSet<Long>()
    private val clock = SimpleDateFormat("HH:mm:ss", Locale.getDefault())

    fun submit(newItems: List<LogStore.Entry>) {
        items.clear()
        items.addAll(newItems)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_log, parent, false)
        return VH(view)
    }

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: VH, position: Int) {
        holder.bind(items[position])
    }

    inner class VH(itemView: View) : RecyclerView.ViewHolder(itemView) {
        private val tvTime: TextView = itemView.findViewById(R.id.tvLogTime)
        private val tvDir: TextView = itemView.findViewById(R.id.tvLogDir)
        private val tvBadge: TextView = itemView.findViewById(R.id.tvLogBadge)
        private val tvSummary: TextView = itemView.findViewById(R.id.tvLogSummary)
        private val tvRaw: TextView = itemView.findViewById(R.id.tvLogRaw)

        fun bind(entry: LogStore.Entry) {
            val context = itemView.context
            tvTime.text = clock.format(Date(entry.time))

            when (entry.dir) {
                LogStore.Dir.TX -> {
                    tvDir.text = "TX"
                    tvDir.setTextColor(ContextCompat.getColor(context, R.color.tx))
                }
                LogStore.Dir.RX -> {
                    tvDir.text = "RX"
                    tvDir.setTextColor(ContextCompat.getColor(context, R.color.rx))
                }
                LogStore.Dir.ERR -> {
                    tvDir.text = "ERR"
                    tvDir.setTextColor(ContextCompat.getColor(context, R.color.err))
                }
            }

            val res = entry.res
            if (entry.dir == LogStore.Dir.RX) {
                tvBadge.visibility = View.VISIBLE
                if (res == null) {
                    tvBadge.text = "原始"
                    tvBadge.setTextColor(ContextCompat.getColor(context, R.color.dim))
                    tvBadge.setBackgroundResource(R.drawable.bg_badge_muted)
                } else if (res.ok) {
                    tvBadge.text = "成功"
                    tvBadge.setTextColor(ContextCompat.getColor(context, R.color.ok))
                    tvBadge.setBackgroundResource(R.drawable.bg_badge_ok)
                } else {
                    tvBadge.text = "失败"
                    tvBadge.setTextColor(ContextCompat.getColor(context, R.color.err))
                    tvBadge.setBackgroundResource(R.drawable.bg_badge_fail)
                }
            } else {
                tvBadge.visibility = View.GONE
            }

            tvSummary.text = when (entry.dir) {
                LogStore.Dir.TX -> Protocol.friendlyRequest(entry.raw)
                LogStore.Dir.RX -> Protocol.friendlyResponse(res, entry.raw)
                LogStore.Dir.ERR -> entry.raw
            }

            val showRaw = expanded.contains(entry.id)
            tvRaw.text = entry.raw
            tvRaw.visibility = if (showRaw) View.VISIBLE else View.GONE

            itemView.setOnClickListener {
                if (showRaw) expanded.remove(entry.id) else expanded.add(entry.id)
                val current = bindingAdapterPosition
                if (current >= 0) notifyItemChanged(current)
            }
        }
    }
}
