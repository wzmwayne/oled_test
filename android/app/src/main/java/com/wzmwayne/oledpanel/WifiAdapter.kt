package com.wzmwayne.oledpanel

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView

class WifiAdapter(
    private val onDelete: (WifiItem) -> Unit
) : RecyclerView.Adapter<WifiAdapter.VH>() {

    private val items = ArrayList<WifiItem>()

    fun submit(newItems: List<WifiItem>) {
        items.clear()
        items.addAll(newItems)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_wifi, parent, false)
        return VH(view)
    }

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: VH, position: Int) {
        holder.bind(items[position])
    }

    inner class VH(itemView: View) : RecyclerView.ViewHolder(itemView) {
        private val tvSsid: TextView = itemView.findViewById(R.id.tvWifiSsid)
        private val tvPass: TextView = itemView.findViewById(R.id.tvWifiPass)
        private val tvAction: TextView = itemView.findViewById(R.id.tvWifiAction)

        fun bind(item: WifiItem) {
            tvSsid.text = item.ssid
            tvPass.text = if (item.pass.isEmpty()) {
                "开放网络"
            } else {
                "密码 " + "•".repeat(item.pass.length.coerceAtMost(12))
            }
            tvAction.text = "删除"
            itemView.setOnClickListener { onDelete(item) }
        }
    }
}
